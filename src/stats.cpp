#include "stats.hpp"

#include <algorithm>

#include "ttlive/events.hpp"

void StatsCollector::reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    start_ = Clock::now();
    active_ = true;
    viewers_ = peakViewers_ = 0;
    diamonds_ = 0;
    likesTotal_ = likesLocal_ = 0;
    likesBase_ = -1;
    comments_ = joins_ = follows_ = shares_ = subscribes_ = 0;
    chatters_.clear();
    gifters_.clear();
    gifts_.clear();
    gifterMap_.clear();

    // The series vectors are read by the UI thread without the lock, and
    // reset() can run on the client thread (Connect event) — defer clearing
    // to sample(), which runs on the UI thread.
    seriesReset_ = true;
}

double StatsCollector::elapsed() const {
    return std::chrono::duration<double>(Clock::now() - start_).count();
}

void StatsCollector::record(const ttlive::Event& e) {
    using ttlive::EventType;
    if (e.type == EventType::Connect) {
        reset();
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    switch (e.type) {
        case EventType::Comment:
            comments_++;
            if (e.user.id) chatters_.insert(e.user.id);
            break;
        case EventType::Gift: {
            // Streakable gifts (type 1) send intermediate updates while the
            // combo runs (repeat_end == 0) and a final message with the full
            // count — count only the final one. Non-streakable gifts NEVER
            // send a final message (repeat_end stays 0), so gating them on
            // the streak flag would drop them entirely; count them at once.
            if (e.gift_type == 1 && e.gift_streaking) break;
            int64_t units = e.repeat_count > 0 ? e.repeat_count : 1;
            int64_t value = (int64_t)e.diamond_count * units;
            diamonds_ += value;
            GiftStat& g = gifts_[e.gift_id];
            g.id = e.gift_id;
            if (!e.gift_name.empty()) g.name = e.gift_name;
            if (e.diamond_count > 0) g.price = e.diamond_count;
            g.count += units;
            g.diamonds += value;
            if (e.user.id) {
                gifters_.insert(e.user.id);
                GifterStat& p = gifterMap_[e.user.id];
                p.id = e.user.id;
                if (!e.user.nickname.empty()) p.name = e.user.nickname;
                else if (p.name.empty()) p.name = "@" + e.user.unique_id;
                p.diamonds += value;
            }
            break;
        }
        case EventType::Like:
            likesLocal_ += e.like_count > 0 ? e.like_count : 1;
            if (e.total_likes > 0) {
                likesTotal_ = e.total_likes;
                if (likesBase_ < 0) likesBase_ = e.total_likes;
            }
            break;
        case EventType::Join:
            joins_++;
            break;
        case EventType::Follow:
            follows_++;
            break;
        case EventType::Share:
            shares_++;
            break;
        case EventType::Subscribe:
            subscribes_++;
            break;
        case EventType::RoomUserSeq:
            if (e.viewer_count > 0) {
                viewers_ = e.viewer_count;
                peakViewers_ = std::max(peakViewers_, viewers_);
            }
            break;
        default:
            break;
    }
}

void StatsCollector::sample() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (seriesReset_) {
        seriesReset_ = false;
        lastSample_ = -1.0;
        tMin_.clear();
        tSec_.clear();
        viewersS_.clear();
        cumDiamonds_.clear();
        cumLikes_.clear();
        cumComments_.clear();
        cumJoins_.clear();
        diamondsRate_.clear();
        likesRate_.clear();
        commentsRate_.clear();
        joinsRate_.clear();
    }
    if (!active_) return;
    double t = elapsed();
    if (lastSample_ >= 0.0 && t - lastSample_ < 1.0) return;
    lastSample_ = t;

    // Likes since connect: prefer the room's authoritative running total.
    double likes = likesBase_ >= 0 ? (double)(likesTotal_ - likesBase_)
                                   : (double)likesLocal_;

    tSec_.push_back(t);
    tMin_.push_back(t / 60.0);
    viewersS_.push_back((double)viewers_);
    cumDiamonds_.push_back((double)diamonds_);
    cumLikes_.push_back(likes);
    cumComments_.push_back((double)comments_);
    cumJoins_.push_back((double)joins_);

    diamondsRate_.push_back(rate(cumDiamonds_, 60.0));
    likesRate_.push_back(rate(cumLikes_, 60.0));
    commentsRate_.push_back(rate(cumComments_, 60.0));
    joinsRate_.push_back(rate(cumJoins_, 60.0));
}

// Per-minute rate over a trailing window: the discrete derivative of `cum`.
//
// TikTok replays a burst of recent history events right after connect, which
// would show up as a huge fake spike — so the window never reaches into the
// first kSettleSec of the session, and no rate is reported until a little
// after that.
double StatsCollector::rate(const std::vector<double>& cum,
                            double windowSec) const {
    constexpr double kSettleSec = 10.0;
    if (cum.size() < 2) return 0.0;
    double tNow = tSec_.back();
    if (tNow < kSettleSec + 5.0) return 0.0;

    size_t j = cum.size() - 1;
    while (j > 0 && tNow - tSec_[j - 1] <= windowSec &&
           tSec_[j - 1] >= kSettleSec)
        j--;
    double dt = tNow - tSec_[j];
    if (dt < 1.0) dt = 1.0;
    return (cum.back() - cum[j]) / dt * 60.0;
}

StatsTotals StatsCollector::totals() const {
    std::lock_guard<std::mutex> lk(mutex_);
    StatsTotals s;
    s.viewers = viewers_;
    s.peakViewers = peakViewers_;
    s.diamonds = diamonds_;
    s.likes = likesBase_ >= 0 ? likesTotal_ - likesBase_ : likesLocal_;
    s.comments = comments_;
    s.joins = joins_;
    s.follows = follows_;
    s.shares = shares_;
    s.subscribes = subscribes_;
    s.uniqueChatters = (int64_t)chatters_.size();
    s.uniqueGifters = (int64_t)gifters_.size();
    s.elapsedSec = active_ ? elapsed() : 0.0;
    return s;
}

std::vector<GiftStat> StatsCollector::giftStats(bool byCount) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<GiftStat> out;
    out.reserve(gifts_.size());
    for (const auto& [id, g] : gifts_) out.push_back(g);
    std::sort(out.begin(), out.end(),
              [byCount](const GiftStat& a, const GiftStat& b) {
                  if (byCount) {
                      if (a.count != b.count) return a.count > b.count;
                      return a.diamonds > b.diamonds;
                  }
                  if (a.diamonds != b.diamonds) return a.diamonds > b.diamonds;
                  return a.count > b.count;
              });
    return out;
}

std::vector<GifterStat> StatsCollector::topGifters(size_t n) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<GifterStat> out;
    out.reserve(gifterMap_.size());
    for (const auto& [id, p] : gifterMap_) out.push_back(p);
    std::sort(out.begin(), out.end(),
              [](const GifterStat& a, const GifterStat& b) {
                  return a.diamonds > b.diamonds;
              });
    if (out.size() > n) out.resize(n);
    return out;
}
