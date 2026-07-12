#pragma once

// StatsCollector — live-stream metrics for the stats panel.
//
// record() is called from the ttlive client thread for every event (before
// filtering); sample() is called every UI frame and appends one point per
// second to the time series. Counters are guarded by a mutex; the series
// vectors are only touched from the UI thread (sample() + plotting).
//
// Metrics:
//   - viewers over time (+ peak)
//   - diamonds/min       — the "gift derivative": is gifting trending up?
//   - likes/comments/joins per minute (activity)
//   - per-gift breakdown: count, diamonds, share (icons via GiftIconCache)
//   - top gifters, session totals, unique chatters/gifters

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ttlive {
struct Event;
}

struct GiftStat {
    int64_t id = 0;
    std::string name;
    int32_t price = 0;      ///< diamonds per unit
    int64_t count = 0;      ///< units sent (finished streaks)
    int64_t diamonds = 0;   ///< count * price
};

struct GifterStat {
    int64_t id = 0;   ///< user id (for the avatar cache)
    std::string name;
    int64_t diamonds = 0;
};

struct StatsTotals {
    int64_t viewers = 0;
    int64_t peakViewers = 0;
    int64_t diamonds = 0;
    int64_t likes = 0;
    int64_t comments = 0;
    int64_t joins = 0;
    int64_t follows = 0;
    int64_t shares = 0;
    int64_t subscribes = 0;
    int64_t uniqueChatters = 0;
    int64_t uniqueGifters = 0;
    double elapsedSec = 0.0;
};

class StatsCollector {
public:
    /// Any thread. Connect events reset the session.
    void record(const ttlive::Event& e);

    /// UI thread. Appends one sample per second (no-op in between).
    void sample();

    void reset();

    // --- UI-thread accessors -------------------------------------------
    StatsTotals totals() const;
    /// Gift breakdown, sorted by units sent (byCount) or by diamonds.
    std::vector<GiftStat> giftStats(bool byCount = false) const;
    std::vector<GifterStat> topGifters(size_t n) const;

    // Time series (UI thread only). x = minutes since session start.
    const std::vector<double>& tMin() const { return tMin_; }
    const std::vector<double>& viewersSeries() const { return viewersS_; }
    const std::vector<double>& diamondsPerMin() const { return diamondsRate_; }
    const std::vector<double>& likesPerMin() const { return likesRate_; }
    const std::vector<double>& commentsPerMin() const { return commentsRate_; }
    const std::vector<double>& joinsPerMin() const { return joinsRate_; }

private:
    double elapsed() const;
    double rate(const std::vector<double>& cum, double windowSec) const;

    using Clock = std::chrono::steady_clock;

    mutable std::mutex mutex_;  // guards everything below
    Clock::time_point start_ = Clock::now();
    bool active_ = false;
    int64_t viewers_ = 0;
    int64_t peakViewers_ = 0;
    int64_t diamonds_ = 0;
    int64_t likesTotal_ = 0;    ///< authoritative room total (if reported)
    int64_t likesLocal_ = 0;    ///< sum of like_count while connected
    int64_t likesBase_ = -1;    ///< room total at connect (for deltas)
    int64_t comments_ = 0;
    int64_t joins_ = 0;
    int64_t follows_ = 0;
    int64_t shares_ = 0;
    int64_t subscribes_ = 0;
    std::set<int64_t> chatters_;
    std::set<int64_t> gifters_;
    std::map<int64_t, GiftStat> gifts_;         // by gift id
    std::map<int64_t, GifterStat> gifterMap_;   // by user id

    // Series — mutated only in sample() (UI thread), read by the plots.
    bool seriesReset_ = false;
    double lastSample_ = -1.0;
    std::vector<double> tMin_, tSec_;
    std::vector<double> viewersS_;
    std::vector<double> cumDiamonds_, cumLikes_, cumComments_, cumJoins_;
    std::vector<double> diamondsRate_, likesRate_, commentsRate_, joinsRate_;
};
