#pragma once

// LiveSession — runs a ttlive::TikTokLiveClient on a background thread and
// exposes a thread-safe feed of chat items + the stream playback URL for the
// UI thread.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ttlive/client.hpp"

struct ChatItem {
    enum class Kind { Comment, Gift, Like, Join, Follow, Share, Subscribe, System };
    Kind kind = Kind::System;
    int64_t userId = 0;   ///< for the avatar cache (0 = no avatar)
    std::string author;   ///< nickname (@handle)
    std::string text;
};

/// Playback URL for a given quality key (HLS preferred, FLV fallback).
/// An empty `quality` selects the stream's default quality.
std::string streamUrlFor(const ttlive::StreamInfo& info,
                         const std::string& quality = {});

class LiveSession {
public:
    enum class State { Idle, Connecting, Connected, Disconnected, Error };

    LiveSession() = default;
    ~LiveSession() { stop(); }

    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    /// Connect to @username's live room on a background thread.
    void start(const std::string& username);

    /// Disconnect and join the background thread.
    void stop();

    State state() const { return state_.load(); }
    std::string error() const;

    /// Playback URL chosen from the room info (HLS preferred, FLV fallback).
    /// Empty until connected.
    std::string streamUrl() const;

    /// Full stream info (all qualities) from the room. Empty until connected.
    ttlive::StreamInfo streamInfo() const;

    /// The room's gift list (names, diamond prices, icon URLs). Populated on
    /// Connect; empty before that.
    std::vector<ttlive::GiftInfo> giftList() const;

    int64_t viewerCount() const { return viewers_.load(); }
    int64_t totalLikes() const { return totalLikes_.load(); }
    std::string roomTitleUser() const;

    /// Move any chat items that arrived since the last call into `out`.
    /// Returns the number of items appended.
    size_t drainChat(std::vector<ChatItem>& out);

    /// Called for EVERY ttlive event (including Unknown/Control), before any
    /// filtering, on the client thread. Set once before start().
    using EventSink = std::function<void(const ttlive::Event&)>;
    void setEventSink(EventSink sink) { eventSink_ = std::move(sink); }

private:
    void threadMain(std::string username);
    void pushChat(ChatItem item);
    void handleEvent(const ttlive::Event& e);

    std::thread thread_;
    EventSink eventSink_;   // immutable while the thread runs
    std::atomic<State> state_{State::Idle};
    std::atomic<int64_t> viewers_{0};
    std::atomic<int64_t> totalLikes_{0};

    mutable std::mutex mutex_;   // guards everything below
    ttlive::TikTokLiveClient* client_ = nullptr;  // owned by threadMain
    std::deque<ChatItem> pending_;
    ttlive::StreamInfo stream_;
    std::vector<ttlive::GiftInfo> giftList_;
    std::string streamUrl_;
    std::string error_;
    std::string user_;
};
