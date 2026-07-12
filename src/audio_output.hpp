#pragma once

// AudioOutput — plays interleaved f32 PCM through the system default device
// via miniaudio (WASAPI / CoreAudio / ALSA-PulseAudio, all runtime-linked).
//
// Live-stream oriented: a bounded FIFO keeps latency in check — when the
// producer outruns playback the oldest samples are dropped so audio stays
// aligned with the "present latest frame" video policy. Underruns play
// silence.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

struct ma_device;  // fwd (miniaudio)

class AudioOutput {
public:
    AudioOutput() = default;
    ~AudioOutput() { stop(); }

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /// Open the default playback device (f32, `channels`, `sampleRate`).
    /// Returns false if the device could not be started.
    bool start(int sampleRate, int channels);

    /// Stop and close the device; discards buffered samples.
    void stop();

    bool running() const { return device_ != nullptr; }
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

    /// Queue interleaved f32 frames for playback (called by the decode
    /// thread). Drops the oldest samples if the FIFO exceeds ~1 s.
    void write(const float* samples, size_t frameCount);

    /// [0..1] linear volume, applied in the device callback.
    void setVolume(float v) { volume_.store(v); }
    float volume() const { return volume_.load(); }
    void setMuted(bool m) { muted_.store(m); }
    bool muted() const { return muted_.load(); }

private:
    static void dataCallback(ma_device* dev, void* out, const void* in,
                             uint32_t frameCount);

    ma_device* device_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;

    std::mutex mutex_;            // guards fifo_
    std::deque<float> fifo_;      // interleaved samples
    size_t fifoCapSamples_ = 0;   // ~1 s worth

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
};
