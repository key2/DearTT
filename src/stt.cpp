#include "stt.hpp"

#include <chrono>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "voxtral.h"

namespace {
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
    voxtral_model* model = voxtral_model_load_from_file(
        modelPath, nullptr, voxtral_gpu_backend::auto_detect);
    if (!model) {
        std::lock_guard<std::mutex> lk(textMutex_);
        status_ = "failed to load model: " + modelPath;
        return;
    }
    voxtral_context_params params;
    params.gpu = voxtral_gpu_backend::auto_detect;
    voxtral_context* ctx = voxtral_init_from_model(model, params);
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
