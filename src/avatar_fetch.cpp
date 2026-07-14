#include "avatar_fetch.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "http_client.hpp"  // ttlive (vendored) Chrome-fingerprinted HTTPS

namespace fs = std::filesystem;

namespace {

std::string sanitize(const std::string& s) {
    std::string out;
    for (char c : s)
        if (!(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
              c == '"' || c == '<' || c == '>' || c == '|'))
            out += c;
    return out.empty() ? "person" : out;
}

// Unescape the JSON-string escapes TikTok embeds in its HTML (\/ and \uXXXX
// for '/' and '&').
std::string jsonUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == '/') { o += '/'; i++; continue; }
            if (n == 'u' && i + 5 < s.size()) {
                std::string hex = s.substr(i + 2, 4);
                int code = (int)strtol(hex.c_str(), nullptr, 16);
                if (code == 0x2F) { o += '/'; i += 5; continue; }
                if (code == 0x26) { o += '&'; i += 5; continue; }
                if (code == 0x3D) { o += '='; i += 5; continue; }
            }
        }
        o += s[i];
    }
    return o;
}

std::string extractAfter(const std::string& body, const char* key) {
    size_t p = body.find(key);
    if (p == std::string::npos) return {};
    p += std::strlen(key);
    size_t e = body.find('"', p);
    if (e == std::string::npos) return {};
    return jsonUnescape(body.substr(p, e - p));
}

// Resolve a TikTok @username to its avatar image URL by scraping the profile.
std::string resolveTikTokAvatar(ttlive::HttpClient& http,
                                const std::string& userIn) {
    std::string user = userIn;
    if (!user.empty() && user[0] == '@') user.erase(user.begin());
    ttlive::HttpResponse resp;
    try {
        resp = http.get("https://www.tiktok.com/@" + user);
    } catch (...) {
        return {};
    }
    const std::string& b = resp.body;
    for (const char* key : {"\"avatarLarger\":\"", "\"avatarMedium\":\"",
                            "\"avatarThumb\":\""}) {
        std::string u = extractAfter(b, key);
        if (u.rfind("http", 0) == 0) return u;
    }
    // Fallback: the Open Graph image is the profile avatar.
    std::string og = extractAfter(b, "property=\"og:image\" content=\"");
    if (og.rfind("http", 0) == 0) return og;
    return {};
}

std::string guessExt(const std::string& url, const std::string& contentType) {
    auto has = [](const std::string& s, const char* n) {
        return s.find(n) != std::string::npos;
    };
    if (has(contentType, "png") || has(url, ".png")) return ".png";
    if (has(contentType, "webp") || has(url, ".webp")) return ".webp";
    if (has(contentType, "gif") || has(url, ".gif")) return ".gif";
    return ".jpg";
}

// Save bytes as profileDir/avatars/<person><ext>, removing any prior avatar
// for that person. Returns the saved path or empty.
std::string saveAvatarBytes(const std::string& profileDir,
                            const std::string& person, const std::string& bytes,
                            const std::string& ext) {
    if (bytes.empty()) return {};
    std::error_code ec;
    fs::path dir = fs::path(profileDir) / "avatars";
    fs::create_directories(dir, ec);
    std::string base = sanitize(person);
    // Remove previous avatar(s) with any extension.
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) &&
            e.path().stem().string() == base)
            fs::remove(e.path(), ec);
    }
    fs::path dst = dir / (base + ext);
    std::ofstream f(dst, std::ios::binary);
    if (!f) return {};
    f.write(bytes.data(), (std::streamsize)bytes.size());
    return dst.string();
}

}  // namespace

AvatarFetcher::~AvatarFetcher() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void AvatarFetcher::request(const std::string& profileDir,
                            const std::string& person, Source src,
                            const std::string& value) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        jobs_.push_back({profileDir, person, value, src});
        status_ = "fetching picture for " + person + "...";
        if (!thread_.joinable())
            thread_ = std::thread(&AvatarFetcher::worker, this);
    }
    cv_.notify_one();
}

std::vector<AvatarResult> AvatarFetcher::poll() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<AvatarResult> out;
    out.swap(done_);
    return out;
}

std::string AvatarFetcher::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void AvatarFetcher::worker() {
    ttlive::HttpClient http;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        AvatarResult res;
        res.person = job.person;
        std::string bytes, ext = ".jpg";

        if (job.src == Source::File) {
            std::ifstream f(job.value, std::ios::binary);
            if (f) {
                bytes.assign((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
                std::string e = fs::path(job.value).extension().string();
                if (!e.empty()) ext = e;
            }
        } else {
            std::string url = job.value;
            if (job.src == Source::TikTok) url = resolveTikTokAvatar(http, url);
            if (!url.empty()) {
                try {
                    ttlive::HttpResponse resp = http.get(url);
                    if (resp.status == 200) {
                        bytes = resp.body;
                        auto ct = resp.headers.find("content-type");
                        ext = guessExt(url, ct != resp.headers.end()
                                                ? ct->second
                                                : std::string());
                    }
                } catch (...) {
                }
            }
        }

        res.path = saveAvatarBytes(job.profileDir, job.person, bytes, ext);
        res.ok = !res.path.empty();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            done_.push_back(res);
            status_ = res.ok ? ("picture saved for " + res.person)
                             : ("failed to fetch picture for " + res.person);
        }
    }
}

bool decodeImageFileRGBA(const std::string& path, std::vector<uint8_t>& rgba,
                         int& outW, int& outH, int maxDim) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        return false;

    bool ok = false;
    AVCodecContext* dec = nullptr;
    AVFrame* frm = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;
    uint8_t* dst[4] = {};
    int dstLines[4] = {};

    do {
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
                avcodec_receive_frame(dec, frm) >= 0)
                got = true;
            av_packet_unref(pkt);
        }
        if (!got) {
            avcodec_send_packet(dec, nullptr);
            got = avcodec_receive_frame(dec, frm) >= 0;
        }
        if (!got || frm->width <= 0) break;

        int outw = frm->width, outh = frm->height;
        if (maxDim > 0) { outw = maxDim; outh = maxDim; }
        sws = sws_getContext(frm->width, frm->height,
                             (AVPixelFormat)frm->format, outw, outh,
                             AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                             nullptr);
        if (!sws) break;
        if (av_image_alloc(dst, dstLines, outw, outh, AV_PIX_FMT_RGBA, 32) < 0)
            break;
        sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, dstLines);
        rgba.resize((size_t)outw * outh * 4);
        for (int y = 0; y < outh; y++)
            std::memcpy(rgba.data() + (size_t)y * outw * 4,
                        dst[0] + (size_t)y * dstLines[0], (size_t)outw * 4);
        outW = outw;
        outH = outh;
        ok = true;
    } while (false);

    if (dst[0]) av_freep(&dst[0]);
    if (sws) sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frm);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    return ok;
}
