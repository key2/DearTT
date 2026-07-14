#pragma once

// FaceDetector — SCRFD face detection via ONNX Runtime.
//
// Detects all faces in an RGBA frame, returning bounding boxes + 5-point
// landmarks (left_eye, right_eye, nose, mouth_left, mouth_right) which the
// embedder uses for alignment.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct FaceBox {
    float x, y, w, h;       // bounding box in pixel coords of the source frame
    float score;             // detection confidence [0,1]
    float landmarks[10];    // 5 × (x, y): le, re, nose, ml, mr
};

class FaceDetector {
public:
    FaceDetector();
    ~FaceDetector();

    /// Load the SCRFD ONNX model. Returns false on failure.
    bool load(const std::string& onnxPath);

    /// Detect faces in an RGBA frame at the given resolution.
    std::vector<FaceBox> detect(const uint8_t* rgba, int w, int h,
                                float scoreThresh = 0.5f,
                                float nmsThresh = 0.4f) const;

    bool ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
