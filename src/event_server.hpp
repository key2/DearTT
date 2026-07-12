#pragma once

// EventServer — embedded HTTP + WebSocket server (civetweb).
//
//   http://<host>:<port>/      minimal event-viewer website (from web/)
//   ws://<host>:<port>/ws      every TikTok event, forwarded as JSON text
//
// broadcast() is thread-safe and may be called from any thread (it is called
// from the ttlive client thread). Clients that disconnect are pruned on their
// close callback; writes to broken sockets simply fail and are ignored.

#include <mutex>
#include <set>
#include <string>

struct mg_context;
struct mg_connection;

/// Directory containing the served website (web/index.html): next to the
/// executable or the CWD on deployed installs, falling back to the source
/// tree for development builds. Empty if none found.
std::string findWebDir();

/// Resolve a bundled file (e.g. "fonts/Twemoji.Mozilla.ttf") next to the
/// executable, in the CWD, or in the source tree. Empty if not found.
std::string findResource(const std::string& relPath);

class EventServer {
public:
    EventServer() = default;
    ~EventServer() { stop(); }

    EventServer(const EventServer&) = delete;
    EventServer& operator=(const EventServer&) = delete;

    /// Start listening on `port`, serving static files from `webRoot`
    /// (optional) and websockets on /ws. Returns false on failure (port in
    /// use, ...) — the app keeps working without the server.
    bool start(int port, const std::string& webRoot);
    void stop();

    /// Send a text (JSON) message to every connected websocket client.
    void broadcast(const std::string& text);

    bool running() const { return ctx_ != nullptr; }
    int port() const { return port_; }
    int clientCount() const;
    std::string error() const;

private:
    // civetweb websocket callbacks (thin trampolines onto this object).
    static int wsConnect(const mg_connection* conn, void* user);
    static void wsReady(mg_connection* conn, void* user);
    static int wsData(mg_connection* conn, int flags, char* data, size_t len,
                      void* user);
    static void wsClose(const mg_connection* conn, void* user);

    mg_context* ctx_ = nullptr;
    int port_ = 0;

    mutable std::mutex mutex_;  // guards clients_ + error_ and serializes
                                // mg_websocket_write across threads
    std::set<mg_connection*> clients_;
    std::string error_;
};
