#pragma once

// VideoPlayer — pulls a live HLS/FLV stream with FFmpeg on a background
// thread, decodes video, converts frames to RGBA and hands the most recent
// frame to the UI thread (which uploads it to an OpenGL texture).
//
// Live-stream oriented: no seeking, no clock slaving — the network paces the
// decode loop and the UI simply presents the latest decoded frame, which
// keeps latency minimal.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_output.hpp"

class VideoPlayer {
public:
    enum class State { Idle, Opening, Playing, Ended, Error };

    VideoPlayer() = default;
    ~VideoPlayer() { close(); }

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    /// Start streaming from a URL (HLS .m3u8 or FLV). Closes any previous
    /// stream first. Returns immediately; decoding happens on a worker thread.
    void open(const std::string& url);

    /// Stop the worker thread and release everything.
    void close();

    State state() const { return state_.load(); }
    std::string error() const;

    /// Copy the latest decoded RGBA frame into `rgba` (resized as needed) if a
    /// new one arrived since the last call. Returns true when `rgba`, `w`, `h`
    /// and `sar` were updated. `sar` is the pixel (sample) aspect ratio:
    /// display the frame at (w * sar) x h — anamorphic streams have sar != 1.
    bool takeFrame(std::vector<uint8_t>& rgba, int& w, int& h, float& sar);

    /// Tap on the decoded audio (f32 interleaved, native rate/channels),
    /// called from the decode thread. Used to feed speech-to-text. Set once
    /// before open(); must be thread-safe / cheap.
    using AudioTap = std::function<void(const float* interleaved, int frames,
                                        int channels, int sampleRate)>;
    void setAudioTap(AudioTap cb) { audioTap_ = std::move(cb); }

    /// Source dimensions (0 until the first frame is decoded).
    int width() const { return width_.load(); }
    int height() const { return height_.load(); }

    /// Decoded video frames per second (1 s sliding window; 0 until known).
    float fps() const { return fps_.load(); }

    /// Audio playback (volume/mute). running() is true once the stream's
    /// audio track is being played.
    AudioOutput& audio() { return audio_; }

    /// True while FFmpeg blocking I/O should be aborted (used internally).
    bool interrupted() const { return quit_.load(); }

private:
    void threadMain(std::string url);
    void setError(const std::string& msg);

    std::thread thread_;
    AudioTap audioTap_;   // immutable while the decode thread runs
    std::atomic<bool> quit_{false};
    AudioOutput audio_;
    std::atomic<State> state_{State::Idle};
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<float> fps_{0.0f};

    mutable std::mutex mutex_;          // guards frame_ + error_
    std::vector<uint8_t> frame_;        // latest RGBA frame
    int frameW_ = 0, frameH_ = 0;
    float frameSar_ = 1.0f;             // pixel aspect ratio of frame_
    bool frameNew_ = false;
    std::string error_;
};
