#pragma once

// AvatarFetcher — downloads a person's *display* profile picture (never used
// for face matching) from one of three sources, always saving it locally under
// the profile's avatars/ folder:
//   * File    — a local image path (drag & drop)
//   * Url     — a direct image URL
//   * TikTok  — a @username, whose public profile avatar is resolved & fetched
//
// Network work happens on a background thread; results are drained on the UI
// thread via poll().

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AvatarResult {
    std::string person;
    std::string path;   // saved local path (empty on failure)
    bool ok = false;
};

class AvatarFetcher {
public:
    enum class Source { File, Url, TikTok };

    AvatarFetcher() = default;
    ~AvatarFetcher();

    /// Queue a fetch. `value` is a file path, URL, or @username per `src`.
    void request(const std::string& profileDir, const std::string& person,
                 Source src, const std::string& value);

    /// UI thread: completed fetches since the last call.
    std::vector<AvatarResult> poll();

    std::string status() const;

private:
    void worker();

    struct Job {
        std::string profileDir, person, value;
        Source src;
    };

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::deque<Job> jobs_;
    std::vector<AvatarResult> done_;
    std::string status_;
};

/// Decode an image file to RGBA. If maxDim > 0 the output is scaled so its
/// largest side is maxDim (a square maxDim×maxDim for thumbnails). Returns
/// false on failure.
bool decodeImageFileRGBA(const std::string& path, std::vector<uint8_t>& rgba,
                         int& outW, int& outH, int maxDim = 0);
