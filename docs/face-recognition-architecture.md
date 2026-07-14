# Face Detection & Recognition Architecture

## Goal

Detect faces in live video frames, extract bounding-box coordinates, and match
them against a local database of known faces — identifying **who is who** on
screen in real time. Must work on both Linux and Windows without requiring
cloud APIs.

---

## High-Level Pipeline

```
┌─────────────┐     ┌──────────────┐     ┌───────────────┐     ┌──────────────┐
│  VideoPlayer │────▶│ Face Detector│────▶│ Face Embedder │────▶│  Matcher /   │
│ (RGBA frame) │     │  (bboxes)    │     │ (128-d vector)│     │  Database    │
└─────────────┘     └──────────────┘     └───────────────┘     └──────────────┘
                           │                      │                      │
                           ▼                      ▼                      ▼
                     x, y, w, h            embedding float[128]     "person_name"
                     + landmarks           (per detected face)      + confidence
```

1. **Detect** — find all face bounding boxes (+ optional 5-point landmarks for
   alignment) in the current frame.
2. **Align & Embed** — crop each face, align it using the landmarks, run it
   through an embedding network to produce a compact feature vector (typically
   128 or 512 floats).
3. **Match** — compare the embedding against a gallery of known faces (enrolled
   photos) using cosine similarity or L2 distance. If the distance is below a
   threshold, assign the identity; otherwise label as "unknown".

---

## Recommended Libraries

### Option A: ONNX Runtime + Pre-trained ONNX Models (★ Recommended)

| Component | Library | Why |
|-----------|---------|-----|
| **Inference engine** | [ONNX Runtime](https://onnxruntime.ai/) | C++ API, single shared lib, CPU/GPU, Linux + Windows + macOS. MIT license. |
| **Face detection model** | [SCRFD](https://github.com/deepinsight/insightface/tree/master/detection/scrfd) (`.onnx`) | State-of-the-art, multiple sizes (500K → 34M params), outputs bboxes + 5-pt landmarks. |
| **Face embedding model** | [ArcFace / InsightFace](https://github.com/deepinsight/insightface) (`.onnx`) | 128-d or 512-d embeddings; top accuracy on LFW/MegaFace. Pre-trained `.onnx` files available. |
| **Gallery matching** | [hnswlib](https://github.com/nmslib/hnswlib) or brute-force cosine | Header-only C++, approximate nearest-neighbor for large galleries; brute-force is fine for <1000 faces. |
| **Image preprocessing** | Already have FFmpeg `swscale` + raw pixel access | Crop, resize to model input (e.g. 112×112), normalize to float32 BGR. |

**Advantages**:
- Single inference runtime for both detection and recognition.
- Models are just `.onnx` files shipped alongside the executable (no Python, no
  framework install).
- GPU acceleration via CUDA/DirectML/CoreML execution providers with zero code
  changes (just load the provider DLL).
- ONNX Runtime is ~40 MB (shared lib + CPU EP); models are ~5–30 MB each.

**Cross-platform build**:
```bash
# Linux
wget https://github.com/microsoft/onnxruntime/releases/.../onnxruntime-linux-x64-1.18.1.tgz
# Windows (MinGW-compatible or MSVC)
wget https://github.com/microsoft/onnxruntime/releases/.../onnxruntime-win-x64-1.18.1.zip
```
Link against `-lonnxruntime`; ship `libonnxruntime.so` / `onnxruntime.dll`.

---

### Option B: dlib (All-in-One C++ Library)

| Component | Library |
|-----------|---------|
| Detection | `dlib::get_frontal_face_detector()` (HOG) or `dlib::cnn_face_detection_model_v1` (MMOD CNN) |
| Landmarks | `dlib::shape_predictor` (68-point or 5-point `.dat` model) |
| Embedding | `dlib::face_recognition_model_v1` (ResNet, 128-d output) |
| Matching | `dlib::length()` (L2 distance; threshold ~0.6) |

**Advantages**:
- Pure C++ with CMake, compiles on Linux/Windows/macOS out of the box.
- Everything in one library (detection + alignment + embedding).
- Well-documented face_recognition pipeline (Python `face_recognition` lib is
  just a dlib wrapper).

**Disadvantages**:
- HOG detector misses profile/small faces; CNN detector is slow on CPU (~200 ms
  per frame without GPU).
- The CNN models are ~22 MB + 99 MB (recognition).
- No built-in GPU acceleration on Windows without CUDA toolkit installed.
- Accuracy is good but below InsightFace/ArcFace on challenging benchmarks.

---

### Option C: OpenCV DNN Module

| Component | Library |
|-----------|---------|
| Detection | `cv::dnn::readNet("face_detection_yunet_2023mar.onnx")` or SSD Caffe model |
| Embedding | `cv::dnn::readNet("face_recognition_sface_2021dec.onnx")` (SFace, 128-d) |
| Matching | `cv::norm(embed1, embed2, NORM_L2)` |

**Advantages**:
- OpenCV's DNN module loads ONNX/Caffe/TF models; can also use the same SCRFD/
  ArcFace models from Option A.
- OpenCV is well-known, widely available on package managers.

**Disadvantages**:
- OpenCV is a large dependency (~50 MB shared lib) if not already linked.
- DNN module inference is slower than ONNX Runtime (no graph optimizations).
- Tricky to cross-compile for MinGW (MSVC easier); we'd need vcpkg or a
  prebuilt binary.

---

### Option D: MediaPipe (via TFLite)

| Component | Library |
|-----------|---------|
| Detection | MediaPipe Face Detection (BlazeFace, `.tflite`) |
| Landmarks | MediaPipe Face Mesh (478 points, `.tflite`) |
| Embedding | Separate model needed (FaceNet/ArcFace `.tflite`) |

- Requires TensorFlow Lite C API, which is lightweight (~3 MB).
- BlazeFace is extremely fast (<5 ms on CPU).
- But: Face Mesh ≠ face recognition; an additional embedding model is needed.
- Cross-platform but less straightforward CMake integration than ONNX Runtime.

---

## Recommended Architecture for DearTT

```
src/
  face_detector.hpp / .cpp    — wraps ONNX Runtime session for SCRFD
  face_embedder.hpp / .cpp    — wraps ONNX Runtime session for ArcFace
  face_gallery.hpp / .cpp     — enrolled identities (name → embedding[])
  face_tracker.hpp / .cpp     — per-frame pipeline + temporal smoothing
```

### face_detector

```cpp
struct FaceBox {
    float x, y, w, h;       // bounding box (pixels, in frame coords)
    float score;             // detection confidence
    float landmarks[10];    // 5 points × 2 (x,y): left_eye, right_eye,
                            //   nose_tip, mouth_left, mouth_right
};

class FaceDetector {
public:
    bool load(const std::string& onnxPath);  // "models/scrfd_2.5g_kps.onnx"
    std::vector<FaceBox> detect(const uint8_t* rgba, int w, int h,
                                float scoreThreshold = 0.5f);
};
```

### face_embedder

```cpp
class FaceEmbedder {
public:
    bool load(const std::string& onnxPath);  // "models/arcface_r50.onnx"
    // Align + embed a single face crop; returns 128 or 512 floats.
    std::vector<float> embed(const uint8_t* rgba, int w, int h,
                             const float landmarks[10]);
};
```

### face_gallery

```cpp
struct Identity {
    std::string name;
    std::string photoPath;          // source enrollment photo
    std::vector<float> embedding;   // stored feature vector
};

class FaceGallery {
public:
    void load(const std::string& dbDir);  // loads identities.json + embeddings
    void save(const std::string& dbDir);
    void enroll(const std::string& name, const std::string& photoPath,
                const std::vector<float>& embedding);
    // Returns (name, confidence) or ("unknown", 0) if no match.
    std::pair<std::string, float> identify(const std::vector<float>& embedding,
                                           float threshold = 0.4f) const;
};
```

### face_tracker (integration with the video player)

```cpp
class FaceTracker {
public:
    struct TrackedFace {
        FaceBox box;
        std::string identity;
        float confidence;
        int trackId;            // stable across frames (IOU-based tracking)
    };

    void configure(const std::string& modelsDir, const std::string& galleryDir);
    // Called once per decoded frame (or every Nth frame for performance).
    std::vector<TrackedFace> process(const uint8_t* rgba, int w, int h);
};
```

The UI would overlay bounding boxes + names on the video using
`ImDrawList::AddRect` / `AddText`, similar to how the stats overlay works today.

---

## Integration with DearTT's Existing Architecture

```
┌─────────────────────────────────────────────────────────┐
│ Decode Thread (VideoPlayer)                             │
│   av_read_frame → avcodec_send/receive → sws_scale     │
│                                              │          │
│   takeFrame() ─── RGBA + w + h + sar ───────┼──┐       │
└─────────────────────────────────────────────────┼───────┘
                                                  │
┌─────────────────────────────────────────────────┼───────┐
│ Face Analysis Thread (new)                      ▼       │
│   Every Nth frame:                                      │
│     detect(rgba, w, h)  → boxes[]                       │
│     for each box:                                       │
│       embed(crop)       → vec[128]                      │
│       gallery.identify  → name                          │
│   Store results in atomic/mutex-guarded struct          │
└─────────────────────────────────────────────────────────┘
                                                  │
┌─────────────────────────────────────────────────┼───────┐
│ UI Thread (main loop)                           ▼       │
│   Upload frame → GL texture                             │
│   Read tracked faces                                    │
│   Draw bboxes + names via ImDrawList (scaled to display)│
│   Forward identities to WebSocket as JSON events        │
└─────────────────────────────────────────────────────────┘
```

Key design points:
- The face analysis runs on its **own thread** — the video player keeps
  producing frames at full rate regardless.
- The analysis thread processes every 3rd–5th frame (configurable) to stay
  within CPU budget.
- Results are double-buffered: the UI reads the latest completed result while
  the analysis thread works on a new frame.
- IOU (Intersection over Union) tracking stabilizes identities across frames
  even when recognition runs at a lower rate.

---

## Performance Budget (targeting 30 fps video)

| Stage | Model | CPU time (1080p) | Notes |
|-------|-------|-------------------|-------|
| Detection | SCRFD-2.5GF | ~8 ms | Input resized to 640×640 |
| Detection | SCRFD-10GF | ~25 ms | Higher accuracy for small faces |
| Alignment | Affine warp | <1 ms | 5-point → 112×112 crop |
| Embedding | ArcFace-R50 | ~15 ms/face | One forward pass per face |
| Embedding | MobileFaceNet | ~4 ms/face | Lighter, slightly less accurate |
| Matching | Brute-force L2 | <0.1 ms | For gallery <1000 |

**Total for 1 face**: ~23 ms (SCRFD-2.5G + ArcFace-R50)
**Total for 1 face**: ~12 ms (SCRFD-2.5G + MobileFaceNet)

Running on every 3rd frame at 30 fps = one analysis every 100 ms → comfortable.
With GPU (CUDA/DirectML): detection drops to ~2 ms, embedding to ~3 ms.

---

## Enrollment Workflow

1. User adds a photo to `gallery/<name>/` (e.g. `gallery/Alice/photo1.jpg`).
2. On startup or "re-enroll" action, the app:
   - Detects the largest face in each photo.
   - Computes and stores the 128-d embedding in `gallery/identities.json`.
   - Multiple photos per person improve robustness (store all embeddings;
     match the nearest one, or average them into a single centroid).
3. During live playback, each detected face's embedding is compared against all
   enrolled embeddings. Closest match below threshold = identified.

```json
// gallery/identities.json
[
  {
    "name": "Alice",
    "photos": ["Alice/photo1.jpg", "Alice/photo2.jpg"],
    "embeddings": [[0.023, -0.114, ...], [0.019, -0.108, ...]]
  },
  {
    "name": "Bob",
    "photos": ["Bob/front.jpg"],
    "embeddings": [[0.187, 0.042, ...]]
  }
]
```

---

## Files to Ship

```
models/
  scrfd_2.5g_kps.onnx          (~3 MB)   face detection + keypoints
  arcface_r50.onnx              (~250 MB → quantize to int8: ~65 MB)
  # OR for minimal size:
  mobilefacenet.onnx            (~5 MB)   faster, slightly less accurate

gallery/
  identities.json               enrolled names + precomputed embeddings
  Alice/
    photo1.jpg
  Bob/
    photo1.jpg

# Alongside the executable:
libonnxruntime.so / onnxruntime.dll   (~40 MB CPU-only, ~12 MB minimal build)
```

---

## CMake Integration Sketch

```cmake
# --- ONNX Runtime (prebuilt, same pattern as FFmpeg) -----------------------
set(ONNXRUNTIME_DIR "" CACHE PATH "Prebuilt ONNX Runtime (include/ + lib/)")

if(ONNXRUNTIME_DIR)
  find_path(ORT_INCLUDE onnxruntime_cxx_api.h
            HINTS ${ONNXRUNTIME_DIR}/include)
  find_library(ORT_LIB onnxruntime
               HINTS ${ONNXRUNTIME_DIR}/lib)
  add_library(onnxruntime INTERFACE)
  target_include_directories(onnxruntime INTERFACE ${ORT_INCLUDE})
  target_link_libraries(onnxruntime INTERFACE ${ORT_LIB})

  # Face recognition module (only built if ONNX Runtime is provided)
  target_sources(deartt PRIVATE
    src/face_detector.cpp
    src/face_embedder.cpp
    src/face_gallery.cpp
    src/face_tracker.cpp
  )
  target_link_libraries(deartt PRIVATE onnxruntime)
  target_compile_definitions(deartt PRIVATE DEARTT_FACE_RECOGNITION)
endif()
```

Conditional compilation: the face system is opt-in (only active when
`-DONNXRUNTIME_DIR=...` is passed), so builds without it remain unchanged.

---

## Summary of Recommendations

| Criterion | Recommendation |
|-----------|----------------|
| **Best accuracy** | ONNX Runtime + InsightFace (SCRFD + ArcFace) |
| **Easiest integration** | dlib (single lib, everything included, simpler API) |
| **Smallest binary** | TFLite + BlazeFace + MobileFaceNet (~8 MB models total) |
| **Best cross-platform GPU** | ONNX Runtime (CUDA / DirectML / CoreML) |
| **For DearTT** | **Option A** — ONNX Runtime + SCRFD + ArcFace. Same ship-prebuilt pattern as FFmpeg, models are just files like `js/`, and the C++ API is minimal. |

---

## References

- [InsightFace model zoo (ONNX)](https://github.com/deepinsight/insightface/tree/master/model_zoo)
- [SCRFD paper](https://arxiv.org/abs/2105.04714) — Sample and Computation Redistribution for Efficient Face Detection
- [ArcFace paper](https://arxiv.org/abs/1801.07698) — Additive Angular Margin Loss
- [ONNX Runtime C/C++ API docs](https://onnxruntime.ai/docs/api/c/)
- [hnswlib](https://github.com/nmslib/hnswlib) — header-only approximate nearest neighbor
- [dlib face recognition example](http://dlib.net/face_recognition.py.html)
- [OpenCV FaceDetectorYN / FaceRecognizerSF](https://docs.opencv.org/4.x/d0/dd4/tutorial_dnn_face.html)
