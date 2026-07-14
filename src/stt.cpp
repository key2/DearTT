#include "stt.hpp"

#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "voxtral.h"

namespace {

// Wine's winevulkan doesn't implement every extension ggml's Vulkan backend
// asks for, and ggml aborts (not throws) on some of those paths — uncatchable.
// Detect Wine and keep STT on the CPU there.
bool runningUnderWine() {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

constexpr int kSttRate = VOXTRAL_SAMPLE_RATE;   // 16000
// Short windows = low latency. Each window is decoded token-by-token and
// streamed to the UI as it arrives (near real-time), then committed.
constexpr size_t kWindow = (size_t)(kSttRate * 2.5);  // ~2.5 s per window
constexpr size_t kMaxBuffer = (size_t)kSttRate * 20;  // drop if worker lags
}  // namespace

bool SttEngine::start(const std::string& modelPath) {
    if (worker_.joinable()) return false;
    quit_.store(false);
    {
        std::lock_guard<std::mutex> lk(textMutex_);
        status_ = "loading model...";
    }
    worker_ = std::thread(&SttEngine::workerMain, this, modelPath);
    return true;
}

void SttEngine::stop() {
    quit_.store(true);
    audioCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (swr_) { swr_free(&swr_); swr_ = nullptr; }
}

void SttEngine::setEnabled(bool e) {
    enabled_.store(e);
    if (!e) {
        std::lock_guard<std::mutex> lk(audioMutex_);
        buf16k_.clear();
    }
    audioCv_.notify_all();
}

std::string SttEngine::status() const {
    std::lock_guard<std::mutex> lk(textMutex_);
    return status_;
}
std::string SttEngine::transcript() const {
    std::lock_guard<std::mutex> lk(textMutex_);
    if (partial_.empty()) return transcript_;
    if (transcript_.empty()) return partial_;
    return transcript_ + " " + partial_;
}
void SttEngine::clearTranscript() {
    std::lock_guard<std::mutex> lk(textMutex_);
    transcript_.clear();
    partial_.clear();
}

void SttEngine::pushAudio(const float* interleaved, int frames, int channels,
                          int sampleRate) {
    if (!enabled_.load() || !ready_.load() || frames <= 0) return;

    // (Re)build the resampler when the input format changes.
    if (!swr_ || swrInRate_ != sampleRate || swrInCh_ != channels) {
        if (swr_) swr_free(&swr_);
        swr_ = nullptr;
        AVChannelLayout inLayout;
        av_channel_layout_default(&inLayout, channels);
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
        if (swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, kSttRate,
                                &inLayout, AV_SAMPLE_FMT_FLT, sampleRate, 0,
                                nullptr) < 0 ||
            swr_init(swr_) < 0) {
            if (swr_) { swr_free(&swr_); swr_ = nullptr; }
            return;
        }
        swrInRate_ = sampleRate;
        swrInCh_ = channels;
    }

    int maxOut = (int)swr_get_out_samples(swr_, frames);
    if (maxOut <= 0) return;
    std::vector<float> mono((size_t)maxOut);
    uint8_t* outPtr = (uint8_t*)mono.data();
    const uint8_t* inPtr = (const uint8_t*)interleaved;
    int n = swr_convert(swr_, &outPtr, maxOut, &inPtr, frames);
    if (n <= 0) return;

    std::lock_guard<std::mutex> lk(audioMutex_);
    if (buf16k_.size() > kMaxBuffer)  // worker fell behind: drop oldest
        buf16k_.erase(buf16k_.begin(),
                      buf16k_.begin() + (buf16k_.size() - kMaxBuffer / 2));
    buf16k_.insert(buf16k_.end(), mono.begin(), mono.begin() + n);
    if (buf16k_.size() >= kWindow) audioCv_.notify_one();
}

void SttEngine::workerMain(std::string modelPath) {
    // GPU init can throw from inside ggml (e.g. a Vulkan driver advertises a
    // device but createDevice fails with a missing extension — seen under
    // Wine's winevulkan). An uncaught exception here would std::terminate the
    // whole app, so catch it and retry CPU-only.
    voxtral_model* model = nullptr;
    voxtral_context* ctx = nullptr;
    std::vector<voxtral_gpu_backend> attempts = {voxtral_gpu_backend::auto_detect,
                                                 voxtral_gpu_backend::none};
    if (runningUnderWine()) {
        std::fprintf(stderr, "[stt] Wine detected: CPU backend only\n");
        attempts = {voxtral_gpu_backend::none};
    }
    for (voxtral_gpu_backend gpu : attempts) {
        try {
            model = voxtral_model_load_from_file(modelPath, nullptr, gpu);
            if (!model) break;  // file problem: a retry won't help
            voxtral_context_params params;
            params.gpu = gpu;
            ctx = voxtral_init_from_model(model, params);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[stt] backend init failed (%s)%s\n", e.what(),
                         gpu == voxtral_gpu_backend::auto_detect
                             ? ", retrying CPU-only"
                             : "");
            ctx = nullptr;
        }
        if (ctx) break;
        if (model) {
            voxtral_model_free(model);
            model = nullptr;
        }
    }
    if (!model) {
        std::lock_guard<std::mutex> lk(textMutex_);
        status_ = "failed to load model: " + modelPath;
        return;
    }
    if (!ctx) {
        voxtral_model_free(model);
        std::lock_guard<std::mutex> lk(textMutex_);
        status_ = "failed to init context";
        return;
    }
    {
        std::lock_guard<std::mutex> lk(textMutex_);
        status_ = "ready";
    }
    ready_.store(true);

    while (!quit_.load()) {
        std::vector<float> window;
        {
            std::unique_lock<std::mutex> lk(audioMutex_);
            audioCv_.wait(lk, [&] {
                return quit_.load() ||
                       (enabled_.load() && buf16k_.size() >= kWindow);
            });
            if (quit_.load()) break;
            if (!enabled_.load() || buf16k_.size() < kWindow) continue;
            window.assign(buf16k_.begin(), buf16k_.begin() + kWindow);
            buf16k_.erase(buf16k_.begin(), buf16k_.begin() + kWindow);
        }

        voxtral_result result;
        // Stream this window's text token-by-token into `partial_` for
        // real-time display; commit it to `transcript_` when the window ends.
        bool ok = voxtral_transcribe_audio_streaming(
            *ctx, window, /*max_tokens=*/0, result,
            [this](const std::string& p) {
                std::lock_guard<std::mutex> lk(textMutex_);
                partial_ = p;
            });
        {
            std::lock_guard<std::mutex> lk(textMutex_);
            partial_.clear();
            if (ok && !result.text.empty()) {
                if (!transcript_.empty() && transcript_.back() != ' ')
                    transcript_ += ' ';
                transcript_ += result.text;
                if (transcript_.size() > 20000)
                    transcript_.erase(0, transcript_.size() - 16000);
            }
        }
    }

    voxtral_free(ctx);
    voxtral_model_free(model);
    ready_.store(false);
}
