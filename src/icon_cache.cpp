#include "icon_cache.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>

#include "http_client.hpp"  // ttlive internal (vendored) HTTPS client

namespace {

constexpr int kIconSize = 44;  // display size; icons are square on the CDN

struct MemReader {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

int memRead(void* opaque, uint8_t* buf, int len) {
    auto* r = (MemReader*)opaque;
    size_t left = r->size - r->pos;
    if (left == 0) return AVERROR_EOF;
    size_t n = std::min((size_t)len, left);
    std::memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return (int)n;
}

int64_t memSeek(void* opaque, int64_t offset, int whence) {
    auto* r = (MemReader*)opaque;
    if (whence == AVSEEK_SIZE) return (int64_t)r->size;
    size_t base = whence == SEEK_CUR ? r->pos
                : whence == SEEK_END ? r->size
                                     : 0;
    int64_t pos = (int64_t)base + offset;
    if (pos < 0 || pos > (int64_t)r->size) return -1;
    r->pos = (size_t)pos;
    return pos;
}

/// Decode an image (PNG/WebP/JPEG/...) from memory to kIconSize² RGBA using
/// FFmpeg — the same libraries the video player already links.
bool decodeIcon(const std::string& bytes, std::vector<uint8_t>& rgba,
                int& outW, int& outH) {
    MemReader reader{(const uint8_t*)bytes.data(), bytes.size(), 0};
    constexpr int kBufSize = 16 * 1024;
    uint8_t* avioBuf = (uint8_t*)av_malloc(kBufSize);
    if (!avioBuf) return false;
    AVIOContext* avio = avio_alloc_context(avioBuf, kBufSize, 0, &reader,
                                           &memRead, nullptr, &memSeek);
    if (!avio) { av_free(avioBuf); return false; }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) { avio_context_free(&avio); return false; }
    fmt->pb = avio;

    bool ok = false;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    do {
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
            fmt = nullptr;  // freed by avformat_open_input on failure
            break;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) break;
        const AVCodec* codec =
            avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
        if (!codec) break;
        dec = avcodec_alloc_context3(codec);
        if (!dec ||
            avcodec_parameters_to_context(dec, fmt->streams[vi]->codecpar) < 0 ||
            avcodec_open2(dec, codec, nullptr) < 0)
            break;

        bool got = false;
        while (!got && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vi &&
                avcodec_send_packet(dec, pkt) >= 0 &&
                avcodec_receive_frame(dec, frame) >= 0)
                got = true;
            av_packet_unref(pkt);
        }
        if (!got) {  // flush (some image decoders need it)
            avcodec_send_packet(dec, nullptr);
            got = avcodec_receive_frame(dec, frame) >= 0;
        }
        if (!got || frame->width <= 0) break;

        SwsContext* sws = sws_getContext(
            frame->width, frame->height, (AVPixelFormat)frame->format,
            kIconSize, kIconSize, AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr,
            nullptr);
        if (!sws) break;

        uint8_t* dst[4] = {};
        int dstLines[4] = {};
        if (av_image_alloc(dst, dstLines, kIconSize, kIconSize,
                           AV_PIX_FMT_RGBA, 64) >= 0) {
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                      dst, dstLines);
            rgba.resize((size_t)kIconSize * kIconSize * 4);
            for (int y = 0; y < kIconSize; y++)
                std::memcpy(rgba.data() + (size_t)y * kIconSize * 4,
                            dst[0] + (size_t)y * dstLines[0],
                            (size_t)kIconSize * 4);
            outW = outH = kIconSize;
            ok = true;
            av_freep(&dst[0]);
        }
        sws_freeContext(sws);
    } while (false);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }
    return ok;
}

}  // namespace

IconCache::~IconCache() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void IconCache::request(int64_t id, const std::string& url) {
    if (id == 0 || url.empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (known_.size() >= kMaxEntries) return;
    if (!known_.insert(id).second) return;
    queue_.emplace_back(id, url);
    if (!worker_.joinable())
        worker_ = std::thread(&IconCache::workerMain, this);
    cv_.notify_one();
}

void IconCache::workerMain() {
    ttlive::HttpClient http;  // own instance: the client's is not thread-safe
    for (;;) {
        std::pair<int64_t, std::string> job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        Decoded d;
        d.id = job.first;
        try {
            ttlive::HttpResponse resp = http.get(job.second);
            if (resp.status == 200 && !resp.body.empty())
                decodeIcon(resp.body, d.rgba, d.w, d.h);
        } catch (...) {
            // Network failure: leave d.rgba empty (icon simply not shown).
        }
        if (!d.rgba.empty()) {
            std::lock_guard<std::mutex> lk(mutex_);
            ready_.push_back(std::move(d));
        }
    }
}

void IconCache::upload() {
    std::vector<Decoded> batch;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (ready_.empty()) return;
        batch.swap(ready_);
    }
    for (const Decoded& d : batch) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d.w, d.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, d.rgba.data());
        textures_[d.id] = tex;
    }
}

unsigned IconCache::texture(int64_t giftId) const {
    auto it = textures_.find(giftId);
    return it == textures_.end() ? 0 : it->second;
}

void IconCache::shutdown() {
    for (auto& [id, tex] : textures_) {
        GLuint t = tex;
        glDeleteTextures(1, &t);
    }
    textures_.clear();
}
