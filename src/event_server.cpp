#include "event_server.hpp"

#include <cstring>
#include <filesystem>

#include "civetweb.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

}  // namespace

std::string findWebDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path base : {exeDir(), fs::current_path(ec)}) {
        if (base.empty()) continue;
        fs::path p = base / "web";
        if (fs::exists(p / "index.html", ec)) return p.string();
    }
#ifdef DEARTT_WEB_DIR
    // Development fallback: the source tree of the build machine.
    if (fs::exists(fs::path(DEARTT_WEB_DIR) / "index.html", ec))
        return DEARTT_WEB_DIR;
#endif
    return {};
}

std::string findResource(const std::string& relPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path base : {exeDir(), fs::current_path(ec)}) {
        if (base.empty()) continue;
        fs::path p = base / relPath;
        if (fs::exists(p, ec)) return p.string();
    }
#ifdef DEARTT_SOURCE_DIR
    // Development fallback: the source tree of the build machine.
    fs::path p = fs::path(DEARTT_SOURCE_DIR) / relPath;
    if (fs::exists(p, ec)) return p.string();
#endif
    return {};
}

bool EventServer::start(int port, const std::string& webRoot) {
    stop();

    static bool initialized = false;
    if (!initialized) {
        mg_init_library(0);
        initialized = true;
    }

    mg_callbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));

    // The preferred port can be briefly held by a previous instance still
    // shutting down — fall back to the next few ports instead of giving up.
    for (int p = port; p < port + 10 && p < 65536; p++) {
        std::string ports = std::to_string(p);
        const char* options[] = {
            "listening_ports",          ports.c_str(),
            "document_root",            webRoot.empty() ? "." : webRoot.c_str(),
            "enable_directory_listing", "no",
            "num_threads",              "8",
            nullptr,
        };
        ctx_ = mg_start(&callbacks, this, options);
        if (ctx_) {
            mg_set_websocket_handler(ctx_, "/ws", &EventServer::wsConnect,
                                     &EventServer::wsReady,
                                     &EventServer::wsData,
                                     &EventServer::wsClose, this);
            port_ = p;
            return true;
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    error_ = "failed to start web server on ports " + std::to_string(port) +
             "-" + std::to_string(port + 9);
    return false;
}

void EventServer::stop() {
    if (!ctx_) return;
    mg_stop(ctx_);  // joins worker threads; close callbacks have run
    ctx_ = nullptr;
    port_ = 0;
    std::lock_guard<std::mutex> lk(mutex_);
    clients_.clear();
}

void EventServer::broadcast(const std::string& text) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (mg_connection* conn : clients_)
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text.c_str(),
                           text.size());
}

int EventServer::clientCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return (int)clients_.size();
}

std::string EventServer::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

int EventServer::wsConnect(const mg_connection*, void*) {
    return 0;  // accept everyone
}

void EventServer::wsReady(mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    static const char hello[] =
        "{\"type\":\"Hello\",\"data\":{\"app\":\"DearTT\","
        "\"endpoint\":\"/ws\"}}";
    std::lock_guard<std::mutex> lk(self->mutex_);
    self->clients_.insert(conn);
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, hello,
                       sizeof(hello) - 1);
}

int EventServer::wsData(mg_connection*, int flags, char*, size_t, void*) {
    // We don't consume client messages; civetweb answers PING internally.
    // Returning 0 on CLOSE lets civetweb finish the closing handshake.
    return (flags & 0x0F) != MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE;
}

void EventServer::wsClose(const mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    std::lock_guard<std::mutex> lk(self->mutex_);
    self->clients_.erase(const_cast<mg_connection*>(conn));
}
