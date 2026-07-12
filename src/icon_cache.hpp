#pragma once

// IconCache — downloads small images (gift icons, user avatars) on a
// background thread, decodes them with FFmpeg (PNG/WebP/JPEG), and exposes
// them as GL textures. One instance per id-space (gift ids / user ids).
//
//   request()  any thread   — enqueue an id+URL not yet cached
//   upload()   UI thread    — turn decoded images into GL textures
//   texture()  UI thread    — texture id (0 while loading / on failure)

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class IconCache {
public:
    IconCache() = default;
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    /// Queue a download unless this id is already cached/pending. Cheap and
    /// thread-safe — safe to call for every event. No-op on empty url, and
    /// once kMaxEntries ids are known (memory cap for busy rooms).
    void request(int64_t id, const std::string& url);

    /// UI thread: upload finished downloads as GL textures. Call per frame.
    void upload();

    /// UI thread: GL texture for the id, or 0 if (not yet) available.
    unsigned texture(int64_t id) const;

    /// UI thread: destroy GL textures (call before the GL context dies).
    void shutdown();

private:
    static constexpr size_t kMaxEntries = 4096;

    void workerMain();

    struct Decoded {
        int64_t id = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::thread worker_;
    std::deque<std::pair<int64_t, std::string>> queue_;  // id, url
    std::set<int64_t> known_;      // queued, done or failed — never re-queued
    std::vector<Decoded> ready_;   // decoded, waiting for GL upload

    std::map<int64_t, unsigned> textures_;  // UI thread only
};
