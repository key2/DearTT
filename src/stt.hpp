#pragma once

// SttEngine — live speech-to-text via voxtral.cpp.
//
// Audio is tapped from the video player (native rate, f32 interleaved),
// resampled to 16 kHz mono, and buffered. A background worker loads the
// Voxtral GGUF model once, then transcribes rolling ~N-second windows while
// enabled, appending the text to a transcript the UI reads each frame.
//
// The heavy model load and inference never touch the UI thread.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct voxtral_model;
struct voxtral_context;
struct SwrContext;

class SttEngine {
public:
    SttEngine() = default;
    ~SttEngine() { stop(); }

    SttEngine(const SttEngine&) = delete;
    SttEngine& operator=(const SttEngine&) = delete;

    /// Start the worker (loads the model asynchronously). Returns false only
    /// if a worker is already running.
    bool start(const std::string& modelPath);
    void stop();

    /// Enable/disable transcription. Disabling clears the pending audio so it
    /// doesn't resume with stale sound.
    void setEnabled(bool e);
    bool enabled() const { return enabled_.load(); }

    /// Model loaded and ready to transcribe.
    bool ready() const { return ready_.load(); }

    std::string status() const;
    std::string transcript() const;
    void clearTranscript();

    /// Compute backend actually in use once ready(): "Vulkan", "CUDA",
    /// "Metal" or "CPU". Empty while not ready.
    std::string backend() const;

    /// Decode-thread audio tap: resampled to 16 kHz mono and buffered (only
    /// while enabled + ready). Cheap; safe to call every audio frame.
    void pushAudio(const float* interleaved, int frames, int channels,
                   int sampleRate);

private:
    void workerMain(std::string modelPath);

    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> ready_{false};

    // 16 kHz mono float buffer shared with the worker.
    mutable std::mutex audioMutex_;
    std::condition_variable audioCv_;
    std::vector<float> buf16k_;

    // Resampler state (touched only on the decode thread in pushAudio).
    SwrContext* swr_ = nullptr;
    int swrInRate_ = 0, swrInCh_ = 0;

    mutable std::mutex textMutex_;
    std::string transcript_;   // committed (finished windows)
    std::string partial_;      // current window, streaming in live
    std::string status_ = "idle";
    std::string backend_;      // "" until the model is loaded
};
