#include "face_embedder.hpp"

#include <cmath>
#include <cstring>

#include <onnxruntime_cxx_api.h>

namespace {

// The canonical 112×112 ArcFace alignment targets (left_eye, right_eye,
// nose_tip, mouth_left, mouth_right).
constexpr float kRefLandmarks[5][2] = {
    {38.2946f, 51.6963f},   // left eye
    {73.5318f, 51.5014f},   // right eye
    {56.0252f, 71.7366f},   // nose tip
    {41.5493f, 92.3655f},   // mouth left
    {70.7299f, 92.2041f},   // mouth right
};

// Compute a similarity transform (2×3 affine) from src landmarks to the
// reference alignment targets using a least-squares fit (Umeyama-like).
// M maps src → 112×112 reference.
void estimateAffine(const float src[10], float M[6]) {
    // Simplified: use only the two eyes + nose for a rigid similarity.
    // Full Umeyama is more robust but this works well for frontal faces.
    double sx = 0, sy = 0, dx = 0, dy = 0;
    for (int i = 0; i < 5; i++) {
        sx += src[i * 2];
        sy += src[i * 2 + 1];
        dx += kRefLandmarks[i][0];
        dy += kRefLandmarks[i][1];
    }
    sx /= 5; sy /= 5; dx /= 5; dy /= 5;

    double num = 0, den = 0;
    for (int i = 0; i < 5; i++) {
        double a = src[i * 2] - sx, b = src[i * 2 + 1] - sy;
        double c = kRefLandmarks[i][0] - dx, d = kRefLandmarks[i][1] - dy;
        num += c * a + d * b;
        den += a * a + b * b;
    }
    double scale = den > 1e-8 ? num / den : 1.0;

    // Rotation
    double num2 = 0;
    for (int i = 0; i < 5; i++) {
        double a = src[i * 2] - sx, b = src[i * 2 + 1] - sy;
        double c = kRefLandmarks[i][0] - dx, d = kRefLandmarks[i][1] - dy;
        num2 += c * b - d * a;
    }
    double sinA = den > 1e-8 ? num2 / den : 0.0;
    double cosA = den > 1e-8 ? num / den : 1.0;

    // Scale applied to cos/sin
    double a = cosA, b = sinA;
    double tx = dx - (a * sx + b * sy);
    double ty = dy - (-b * sx + a * sy);

    M[0] = (float)a;  M[1] = (float)b;  M[2] = (float)tx;
    M[3] = (float)-b; M[4] = (float)a;  M[5] = (float)ty;
}

// Warp src RGBA image using affine M (2×3) into a 112×112 BGR float blob.
void warpAffine(const uint8_t* rgba, int srcW, int srcH,
                const float M[6], float* dst, int dstW, int dstH) {
    // Invert M for backward mapping.
    float det = M[0] * M[4] - M[1] * M[3];
    if (std::fabs(det) < 1e-8f) return;
    float invDet = 1.0f / det;
    float iM[6];
    iM[0] = M[4] * invDet;
    iM[1] = -M[1] * invDet;
    iM[2] = (M[1] * M[5] - M[4] * M[2]) * invDet;
    iM[3] = -M[3] * invDet;
    iM[4] = M[0] * invDet;
    iM[5] = (M[3] * M[2] - M[0] * M[5]) * invDet;

    size_t planeSize = (size_t)dstW * dstH;
    float* planeB = dst;
    float* planeG = dst + planeSize;
    float* planeR = dst + 2 * planeSize;

    for (int y = 0; y < dstH; y++) {
        for (int x = 0; x < dstW; x++) {
            float srcX = iM[0] * x + iM[1] * y + iM[2];
            float srcY = iM[3] * x + iM[4] * y + iM[5];
            int sx = (int)srcX, sy = (int)srcY;
            size_t idx = (size_t)y * dstW + x;
            if (sx < 0 || sy < 0 || sx >= srcW - 1 || sy >= srcH - 1) {
                planeB[idx] = planeG[idx] = planeR[idx] = 0.0f;
                continue;
            }
            // Bilinear interpolation.
            float fx = srcX - sx, fy = srcY - sy;
            auto px = [&](int ox, int oy) -> const uint8_t* {
                return rgba + ((size_t)(sy + oy) * srcW + sx + ox) * 4;
            };
            const uint8_t* p00 = px(0, 0);
            const uint8_t* p10 = px(1, 0);
            const uint8_t* p01 = px(0, 1);
            const uint8_t* p11 = px(1, 1);
            for (int c = 0; c < 3; c++) {
                float v = (1 - fx) * (1 - fy) * p00[c] +
                          fx * (1 - fy) * p10[c] +
                          (1 - fx) * fy * p01[c] +
                          fx * fy * p11[c];
                float norm = (v - 127.5f) / 127.5f;
                if (c == 0) planeR[idx] = norm;  // RGBA R → plane R
                else if (c == 1) planeG[idx] = norm;
                else planeB[idx] = norm;
            }
        }
    }
}

}  // namespace

struct FaceEmbedder::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "FaceEmbedder"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName;
    int embDim = 0;
};

FaceEmbedder::FaceEmbedder() : impl_(std::make_unique<Impl>()) {}
FaceEmbedder::~FaceEmbedder() = default;

bool FaceEmbedder::load(const std::string& onnxPath) {
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
        auto outName = impl_->session->GetOutputNameAllocated(0, alloc);
        impl_->outputName = outName.get();

        // Output shape: [1, embDim]
        auto outInfo = impl_->session->GetOutputTypeInfo(0);
        auto shape = outInfo.GetTensorTypeAndShapeInfo().GetShape();
        impl_->embDim = (int)shape.back();
        return true;
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[FaceEmbedder] load failed: %s\n", e.what());
        return false;
    }
}

bool FaceEmbedder::ready() const { return impl_->session != nullptr; }
int FaceEmbedder::dim() const { return impl_->embDim; }

std::vector<float> FaceEmbedder::embed(const uint8_t* rgba, int w, int h,
                                       const float landmarks[10]) const {
    if (!impl_->session) return {};

    constexpr int kSize = 112;

    // Compute alignment transform from detected landmarks to canonical pose.
    float M[6];
    estimateAffine(landmarks, M);

    // Warp + normalize → (1, 3, 112, 112) float blob.
    std::vector<float> blob((size_t)3 * kSize * kSize, 0.0f);
    warpAffine(rgba, w, h, M, blob.data(), kSize, kSize);

    int64_t inputShape[] = {1, 3, kSize, kSize};
    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, blob.data(), blob.size(), inputShape, 4);

    const char* inNames[] = {impl_->inputName.c_str()};
    const char* outNames[] = {impl_->outputName.c_str()};

    auto outputs = impl_->session->Run(
        Ort::RunOptions{nullptr}, inNames, &inputTensor, 1, outNames, 1);

    const float* data = outputs[0].GetTensorData<float>();
    std::vector<float> emb(data, data + impl_->embDim);

    // L2 normalize.
    float norm = 0.0f;
    for (float v : emb) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-6f)
        for (float& v : emb) v /= norm;

    return emb;
}
