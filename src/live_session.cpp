#include "live_session.hpp"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// Prefer HLS for the requested quality; fall back to FLV. FFmpeg plays both.
// Empty `quality` = the stream's default quality (with any-quality fallback).
std::string streamUrlFor(const ttlive::StreamInfo& s,
                         const std::string& quality) {
    std::string want = quality.empty() ? s.default_quality : quality;

    // 1. Exact quality match: HLS first, then FLV.
    for (const auto& q : s.qualities) {
        if (q.quality != want) continue;
        if (!q.hls_url.empty()) return q.hls_url;
        if (!q.flv_url.empty()) return q.flv_url;
    }
    // An explicit quality that has no URL: no silent substitution.
    if (!quality.empty()) return {};

    // 2. Default missing: any HLS, then best FLV, then RTMP.
    for (const auto& q : s.qualities)
        if (!q.hls_url.empty()) return q.hls_url;
    std::string flv = s.best_flv();
    if (!flv.empty()) return flv;
    return s.rtmp_pull_url;
}

namespace {

std::filesystem::path exeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0)
        return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

// The QuickJS signer loads TikTok's SDK JS at runtime. The compile-time
// default points into the build machine's source tree, which doesn't exist
// on deployed/packaged installs — prefer a "js" directory next to the
// executable (or in the CWD) when present.
std::string findJsDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path base : {exeDir(), fs::current_path(ec)}) {
        if (base.empty()) continue;
        for (const char* sub : {"js", "ttlive-cpp/js"}) {
            fs::path p = base / sub;
            if (fs::exists(p / "webmssdk.js", ec))
                return p.string();
        }
    }
    return {};  // fall back to the compile-time default
}

std::string who(const ttlive::User& u) {
    if (!u.nickname.empty() && !u.unique_id.empty() &&
        u.nickname != u.unique_id)
        return u.nickname + " (@" + u.unique_id + ")";
    if (!u.nickname.empty()) return u.nickname;
    if (!u.unique_id.empty()) return "@" + u.unique_id;
    return "someone";
}

}  // namespace

void LiveSession::start(const std::string& username) {
    stop();
    state_.store(State::Connecting);
    viewers_.store(0);
    totalLikes_.store(0);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_.clear();
        stream_ = {};
        giftList_.clear();
        streamUrl_.clear();
        error_.clear();
        user_ = username;
    }
    thread_ = std::thread(&LiveSession::threadMain, this, username);
}

void LiveSession::stop() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (client_) client_->disconnect();
    }
    if (thread_.joinable()) thread_.join();
    state_.store(State::Idle);
}

std::string LiveSession::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

std::string LiveSession::streamUrl() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return streamUrl_;
}

ttlive::StreamInfo LiveSession::streamInfo() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stream_;
}

std::vector<ttlive::GiftInfo> LiveSession::giftList() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return giftList_;
}

std::string LiveSession::roomTitleUser() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return user_;
}

size_t LiveSession::drainChat(std::vector<ChatItem>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    size_t n = pending_.size();
    for (auto& item : pending_) out.push_back(std::move(item));
    pending_.clear();
    return n;
}

void LiveSession::pushChat(ChatItem item) {
    std::lock_guard<std::mutex> lk(mutex_);
    pending_.push_back(std::move(item));
    // Don't let the queue grow unbounded if the UI stalls.
    while (pending_.size() > 2000) pending_.pop_front();
}

void LiveSession::handleEvent(const ttlive::Event& e) {
    using ttlive::EventType;

    // Forward every event (100%, incl. Unknown/Control) before any filtering.
    if (eventSink_) eventSink_(e);

    switch (e.type) {
        case EventType::Connect: {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                stream_ = e.stream;
                streamUrl_ = streamUrlFor(e.stream, {});
                // Fetched before Connect is emitted (fetch_gift_list=true);
                // we're on the client thread, so the read is safe here.
                if (client_) giftList_ = client_->gift_list();
            }
            state_.store(State::Connected);
            pushChat({ChatItem::Kind::System, 0, "",
                      "Connected to @" + e.unique_id + " (room " +
                          std::to_string(e.room_id) + ")"});
            break;
        }
        case EventType::Comment:
            pushChat({ChatItem::Kind::Comment, e.user.id, who(e.user), e.comment});
            break;
        case EventType::Gift:
            // Report streakable (type 1) combos once, when finished. Gifts of
            // any other type never get a "finished" message (repeat_end stays
            // 0), so they are reported immediately.
            if (!(e.gift_type == 1 && e.gift_streaking)) {
                std::string t = "sent " + e.gift_name;
                if (e.repeat_count > 1)
                    t += " x" + std::to_string(e.repeat_count);
                if (e.diamond_count > 0)
                    t += " (" +
                         std::to_string((int64_t)e.diamond_count *
                                        (e.repeat_count > 0 ? e.repeat_count
                                                            : 1)) +
                         " diamonds)";
                pushChat({ChatItem::Kind::Gift, e.user.id, who(e.user), t});
            }
            break;
        case EventType::Like:
            if (e.total_likes > 0) totalLikes_.store(e.total_likes);
            break;  // too chatty for the feed; shown in the header
        case EventType::Join:
            pushChat({ChatItem::Kind::Join, e.user.id, who(e.user), "joined"});
            break;
        case EventType::Follow:
            pushChat({ChatItem::Kind::Follow, e.user.id, who(e.user), "followed the host"});
            break;
        case EventType::Share:
            pushChat({ChatItem::Kind::Share, e.user.id, who(e.user), "shared the LIVE"});
            break;
        case EventType::Subscribe:
            pushChat({ChatItem::Kind::Subscribe, e.user.id, who(e.user), "subscribed"});
            break;
        case EventType::RoomUserSeq:
            if (e.viewer_count > 0) viewers_.store(e.viewer_count);
            break;
        case EventType::LiveEnd:
            pushChat({ChatItem::Kind::System, 0, "", "The LIVE has ended."});
            break;
        default:
            break;
    }
}

void LiveSession::threadMain(std::string username) {
    try {
        ttlive::ClientOptions opts;
        opts.js_dir = findJsDir();
        opts.fetch_gift_list = true;  // names/prices/icons for the stats panel
        // Dual transport: run the WebSocket (low latency, auto-reconnect on
        // drop) AND HTTP long-polling concurrently. Messages are de-duplicated
        // by server msg_id, so counts stay accurate even if the socket drops
        // or one transport misses a message. Override with
        // DEARTT_TRANSPORT=ws|poll|dual (default dual).
        opts.use_websocket = true;
        opts.use_polling = true;
        if (const char* tr = std::getenv("DEARTT_TRANSPORT")) {
            std::string t = tr;
            opts.use_websocket = (t == "ws" || t == "dual");
            opts.use_polling = (t == "poll" || t == "dual");
            if (!opts.use_websocket && !opts.use_polling)
                opts.use_websocket = true;  // never disable both
        }
        // Age-restricted LIVEs only hand out stream URLs to logged-in
        // sessions (room/info status_code 4003110 otherwise). Users can pass
        // their browser session, e.g. DEARTT_COOKIES="sessionid=...".
        if (const char* c = std::getenv("DEARTT_COOKIES")) opts.cookies = c;
        ttlive::TikTokLiveClient client(username, opts);
        client.on_any([this](const ttlive::Event& e) { handleEvent(e); });
        {
            std::lock_guard<std::mutex> lk(mutex_);
            client_ = &client;
        }
        // Blocks until the LIVE genuinely ends or disconnect() is called;
        // transient WebSocket drops are auto-reconnected inside run().
        client.run();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            client_ = nullptr;
        }
        state_.store(State::Disconnected);
        pushChat({ChatItem::Kind::System, 0, "", "Disconnected."});
    } catch (const std::exception& ex) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            client_ = nullptr;
            error_ = ex.what();
        }
        state_.store(State::Error);
        pushChat({ChatItem::Kind::System, 0, "",
                  std::string("Connection failed: ") + ex.what()});
    }
}
