#pragma once

// FaceEmbedder — ArcFace/MobileFaceNet embedding via ONNX Runtime.
//
// Given a face crop + 5-point landmarks, aligns the face to a canonical
// 112×112 pose and produces a 512-d (or 128-d) normalized feature vector.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FaceEmbedder {
public:
    FaceEmbedder();
    ~FaceEmbedder();

    bool load(const std::string& onnxPath);
    bool ready() const;

    /// Embedding dimension (512 for w600k_mbf).
    int dim() const;

    /// Align the face using 5-point landmarks and compute its embedding.
    /// `rgba` is the full frame; landmarks are in frame pixel coordinates.
    std::vector<float> embed(const uint8_t* rgba, int w, int h,
                             const float landmarks[10]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
