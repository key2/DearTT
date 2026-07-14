#include "face_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

#include <onnxruntime_cxx_api.h>

namespace {

// SCRFD strides and anchor counts for the 3 detection heads.
constexpr int kStrides[] = {8, 16, 32};
constexpr int kNumStrides = 3;

struct Anchor {
    float cx, cy;
};

std::vector<Anchor> generateAnchors(int fh, int fw, int stride) {
    std::vector<Anchor> anchors;
    anchors.reserve((size_t)fh * fw * 2);
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            float cx = (x + 0.5f) * stride;
            float cy = (y + 0.5f) * stride;
            anchors.push_back({cx, cy});
            anchors.push_back({cx, cy});  // 2 anchors per cell
        }
    return anchors;
}

float iou(const FaceBox& a, const FaceBox& b) {
    float x0 = std::max(a.x, b.x);
    float y0 = std::max(a.y, b.y);
    float x1 = std::min(a.x + a.w, b.x + b.w);
    float y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return 0.0f;
    float inter = (x1 - x0) * (y1 - y0);
    float areaA = a.w * a.h, areaB = b.w * b.h;
    return inter / (areaA + areaB - inter);
}

std::vector<FaceBox> nms(std::vector<FaceBox>& dets, float threshold) {
    std::sort(dets.begin(), dets.end(),
              [](const FaceBox& a, const FaceBox& b) {
                  return a.score > b.score;
              });
    std::vector<bool> suppressed(dets.size(), false);
    std::vector<FaceBox> result;
    for (size_t i = 0; i < dets.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!suppressed[j] && iou(dets[i], dets[j]) > threshold)
                suppressed[j] = true;
        }
    }
    return result;
}

}  // namespace

struct FaceDetector::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "FaceDetector"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::vector<std::string> outputNames;
    int inputH = 640, inputW = 640;
};

FaceDetector::FaceDetector() : impl_(std::make_unique<Impl>()) {}
FaceDetector::~FaceDetector() = default;

bool FaceDetector::load(const std::string& onnxPath) {
    try {
        impl_->opts.SetIntraOpNumThreads(2);
        impl_->opts.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        std::wstring wpath(onnxPath.begin(), onnxPath.end());
        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, wpath.c_str(), impl_->opts);
#else
        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, onnxPath.c_str(), impl_->opts);
#endif

        Ort::AllocatorWithDefaultOptions alloc;
        auto inName = impl_->session->GetInputNameAllocated(0, alloc);
        impl_->inputName = inName.get();

        size_t numOutputs = impl_->session->GetOutputCount();
        for (size_t i = 0; i < numOutputs; i++) {
            auto name = impl_->session->GetOutputNameAllocated(i, alloc);
            impl_->outputNames.push_back(name.get());
        }
        return true;
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[FaceDetector] load failed: %s\n", e.what());
        return false;
    }
}

bool FaceDetector::ready() const {
    return impl_->session != nullptr;
}

std::vector<FaceBox> FaceDetector::detect(const uint8_t* rgba, int srcW,
                                          int srcH, float scoreThresh,
                                          float nmsThresh) const {
    if (!impl_->session) return {};

    const int inW = impl_->inputW, inH = impl_->inputH;
    float scaleX = (float)srcW / inW;
    float scaleY = (float)srcH / inH;

    // Preprocess: RGBA → float32 BGR, resize to inW×inH, normalize.
    std::vector<float> blob((size_t)3 * inH * inW);
    float* bR = blob.data();
    float* bG = bR + (size_t)inH * inW;
    float* bB = bG + (size_t)inH * inW;

    for (int y = 0; y < inH; y++) {
        int srcY = (int)(y * scaleY);
        if (srcY >= srcH) srcY = srcH - 1;
        for (int x = 0; x < inW; x++) {
            int srcX = (int)(x * scaleX);
            if (srcX >= srcW) srcX = srcW - 1;
            const uint8_t* px = rgba + ((size_t)srcY * srcW + srcX) * 4;
            size_t idx = (size_t)y * inW + x;
            bB[idx] = (px[2] - 127.5f) / 128.0f;
            bG[idx] = (px[1] - 127.5f) / 128.0f;
            bR[idx] = (px[0] - 127.5f) / 128.0f;
        }
    }

    // Run inference.
    int64_t inputShape[] = {1, 3, inH, inW};
    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, blob.data(), blob.size(), inputShape, 4);

    const char* inNames[] = {impl_->inputName.c_str()};
    std::vector<const char*> outNames;
    for (const auto& n : impl_->outputNames) outNames.push_back(n.c_str());

    auto outputs = impl_->session->Run(
        Ort::RunOptions{nullptr}, inNames, &inputTensor, 1,
        outNames.data(), outNames.size());

    // Parse SCRFD outputs: 3 strides × (scores, boxes, landmarks).
    // Output order: score8, score16, score32, box8, box16, box32, kps8, kps16, kps32
    std::vector<FaceBox> dets;

    for (int si = 0; si < kNumStrides; si++) {
        int stride = kStrides[si];
        int fh = inH / stride, fw = inW / stride;
        auto anchors = generateAnchors(fh, fw, stride);

        const float* scores = outputs[si].GetTensorData<float>();
        const float* boxes = outputs[kNumStrides + si].GetTensorData<float>();
        const float* kps = outputs[2 * kNumStrides + si].GetTensorData<float>();
        size_t count = anchors.size();

        for (size_t i = 0; i < count; i++) {
            if (scores[i] < scoreThresh) continue;
            float cx = anchors[i].cx, cy = anchors[i].cy;

            // boxes: distance from anchor center to left, top, right, bottom
            float l = boxes[i * 4 + 0] * stride;
            float t = boxes[i * 4 + 1] * stride;
            float r = boxes[i * 4 + 2] * stride;
            float b = boxes[i * 4 + 3] * stride;

            FaceBox det;
            det.x = (cx - l) * scaleX;
            det.y = (cy - t) * scaleY;
            det.w = (l + r) * scaleX;
            det.h = (t + b) * scaleY;
            det.score = scores[i];

            // 5 keypoints (relative to anchor center, scaled by stride)
            for (int k = 0; k < 5; k++) {
                det.landmarks[k * 2 + 0] =
                    (kps[i * 10 + k * 2 + 0] * stride + cx) * scaleX;
                det.landmarks[k * 2 + 1] =
                    (kps[i * 10 + k * 2 + 1] * stride + cy) * scaleY;
            }
            dets.push_back(det);
        }
    }

    return nms(dets, nmsThresh);
}
