#include "audio_output.hpp"

#include <cstring>

#include "miniaudio.h"

bool AudioOutput::start(int sampleRate, int channels) {
    stop();
    if (sampleRate <= 0 || channels <= 0) return false;

    auto* dev = new ma_device;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = (ma_uint32)channels;
    cfg.sampleRate = (ma_uint32)sampleRate;
    cfg.dataCallback = &AudioOutput::dataCallback;
    cfg.pUserData = this;

    if (ma_device_init(nullptr, &cfg, dev) != MA_SUCCESS) {
        delete dev;
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        ma_device_uninit(dev);
        delete dev;
        return false;
    }

    sampleRate_ = sampleRate;
    channels_ = channels;
    fifoCapSamples_ = (size_t)sampleRate * channels;  // ~1 s
    device_ = dev;
    return true;
}

void AudioOutput::stop() {
    if (device_) {
        ma_device_uninit(device_);  // stops + joins the audio thread
        delete device_;
        device_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    fifo_.clear();
}

void AudioOutput::write(const float* samples, size_t frameCount) {
    if (!device_ || frameCount == 0) return;
    const size_t n = frameCount * (size_t)channels_;
    std::lock_guard<std::mutex> lk(mutex_);
    fifo_.insert(fifo_.end(), samples, samples + n);
    // Bound the FIFO: drop the oldest samples so latency stays ~<= 1 s and
    // the audio track follows the freshest video frames.
    if (fifo_.size() > fifoCapSamples_) {
        size_t drop = fifo_.size() - fifoCapSamples_;
        drop -= drop % channels_;  // keep frame alignment
        fifo_.erase(fifo_.begin(), fifo_.begin() + drop);
    }
}

void AudioOutput::dataCallback(ma_device* dev, void* out, const void*,
                               uint32_t frameCount) {
    auto* self = static_cast<AudioOutput*>(dev->pUserData);
    float* dst = static_cast<float*>(out);
    const size_t want = (size_t)frameCount * self->channels_;

    size_t got = 0;
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        got = self->fifo_.size() < want ? self->fifo_.size() : want;
        for (size_t i = 0; i < got; ++i) dst[i] = self->fifo_[i];
        self->fifo_.erase(self->fifo_.begin(), self->fifo_.begin() + got);
    }
    if (got < want) std::memset(dst + got, 0, (want - got) * sizeof(float));

    const float vol = self->muted_.load() ? 0.0f : self->volume_.load();
    if (vol != 1.0f)
        for (size_t i = 0; i < got; ++i) dst[i] *= vol;
}
