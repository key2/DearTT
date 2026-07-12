#include "video_player.hpp"

#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

// Present a browser-ish UA: TikTok's CDN occasionally rejects the default
// "Lavf/xx" user agent.
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

int interruptCb(void* opaque) {
    auto* self = static_cast<VideoPlayer*>(opaque);
    return self->interrupted() ? 1 : 0;
}

std::string avErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

void VideoPlayer::open(const std::string& url) {
    close();
    quit_.store(false);
    state_.store(State::Opening);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        error_.clear();
        frameNew_ = false;
    }
    thread_ = std::thread(&VideoPlayer::threadMain, this, url);
}

void VideoPlayer::close() {
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    audio_.stop();
    state_.store(State::Idle);
    width_.store(0);
    height_.store(0);
    fps_.store(0.0f);
    std::lock_guard<std::mutex> lk(mutex_);
    frame_.clear();
    frameNew_ = false;
}

std::string VideoPlayer::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

void VideoPlayer::setError(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = msg;
    }
    state_.store(State::Error);
}

bool VideoPlayer::takeFrame(std::vector<uint8_t>& rgba, int& w, int& h,
                            float& sar) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!frameNew_) return false;
    rgba = frame_;          // copy; UI uploads it right away
    w = frameW_;
    h = frameH_;
    sar = frameSar_;
    frameNew_ = false;
    return true;
}

void VideoPlayer::threadMain(std::string url) {
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) { setError("avformat_alloc_context failed"); return; }
    fmt->interrupt_callback.callback = interruptCb;
    fmt->interrupt_callback.opaque = this;

    // Low-latency live tuning.
    fmt->flags |= AVFMT_FLAG_NOBUFFER;
    fmt->probesize = 1 << 20;             // 1 MB is plenty for FLV/HLS
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "user_agent", kUserAgent, 0);
    av_dict_set(&opts, "referer", "https://www.tiktok.com/", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);       // 15 s I/O timeout
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "protocol_whitelist",
                "file,http,https,tcp,tls,crypto,hls,applehttp", 0);

    int err = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        setError("open failed: " + avErr(err));
        avformat_free_context(fmt);
        return;
    }

    err = avformat_find_stream_info(fmt, nullptr);
    if (err < 0) {
        setError("find_stream_info failed: " + avErr(err));
        avformat_close_input(&fmt);
        return;
    }

    const AVCodec* codec = nullptr;
    int videoIdx =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoIdx < 0 || !codec) {
        setError("no video stream found");
        avformat_close_input(&fmt);
        return;
    }

    // Audio is best-effort: a missing/broken track never fails the video.
    const AVCodec* acodec = nullptr;
    int audioIdx =
        av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, videoIdx, &acodec, 0);
    AVCodecContext* adec = nullptr;
    if (audioIdx >= 0 && acodec) {
        adec = avcodec_alloc_context3(acodec);
        avcodec_parameters_to_context(adec,
                                      fmt->streams[audioIdx]->codecpar);
        if (avcodec_open2(adec, acodec, nullptr) < 0)
            avcodec_free_context(&adec);  // video-only playback
    }

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[videoIdx]->codecpar);
    dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // Multi-threaded decode. libavcodec defaults to a single thread unless
    // asked otherwise (the ffmpeg CLI sets this too); 0 = one per core.
    dec->thread_count = 0;
    dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    err = avcodec_open2(dec, codec, nullptr);
    if (err < 0) {
        setError("decoder open failed: " + avErr(err));
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    SwsContext* sws = nullptr;
    int swsW = 0, swsH = 0;
    AVPixelFormat swsFmt = AV_PIX_FMT_NONE;
    // sws_scale destination: allocated with av_image_alloc so it has the
    // alignment + padding FFmpeg's SIMD paths require (a bare std::vector
    // buffer gets overrun by the wider writes -> heap corruption).
    uint8_t* dstData[4] = {};
    int dstStride[4] = {};

    // Audio: decoded frames -> f32 stereo (same rate; the device converts
    // further if needed) -> AudioOutput FIFO.
    AVFrame* afrm = adec ? av_frame_alloc() : nullptr;
    SwrContext* swr = nullptr;
    std::vector<float> pcm;
    const int outChannels = 2;

    state_.store(State::Playing);

    // Decoded-fps measurement (1 s sliding window).
    auto fpsWindowStart = std::chrono::steady_clock::now();
    int fpsCount = 0;

    while (!quit_.load()) {
        err = av_read_frame(fmt, pkt);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                state_.store(State::Ended);
            } else if (!quit_.load()) {
                setError("stream read failed: " + avErr(err));
            }
            break;
        }
        if (adec && pkt->stream_index == audioIdx) {
            err = avcodec_send_packet(adec, pkt);
            av_packet_unref(pkt);
            if (err < 0 && err != AVERROR(EAGAIN)) continue;

            while (avcodec_receive_frame(adec, afrm) == 0) {
                if (!swr) {
                    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
                    if (swr_alloc_set_opts2(&swr, &outLayout,
                                            AV_SAMPLE_FMT_FLT,
                                            afrm->sample_rate,
                                            &afrm->ch_layout,
                                            (AVSampleFormat)afrm->format,
                                            afrm->sample_rate, 0,
                                            nullptr) < 0 ||
                        swr_init(swr) < 0) {
                        if (swr) swr_free(&swr);
                        avcodec_free_context(&adec);  // give up on audio
                        break;
                    }
                    audio_.start(afrm->sample_rate, outChannels);
                }
                if (!adec) break;

                const int maxOut =
                    (int)swr_get_out_samples(swr, afrm->nb_samples);
                pcm.resize((size_t)maxOut * outChannels);
                uint8_t* outPtr = (uint8_t*)pcm.data();
                int n = swr_convert(swr, &outPtr, maxOut,
                                    (const uint8_t**)afrm->extended_data,
                                    afrm->nb_samples);
                if (n > 0 && audio_.running())
                    audio_.write(pcm.data(), (size_t)n);
                av_frame_unref(afrm);
            }
            continue;
        }
        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }
        err = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (err < 0 && err != AVERROR(EAGAIN)) continue;

        while (avcodec_receive_frame(dec, frm) == 0) {
            if (!sws || swsW != frm->width || swsH != frm->height ||
                swsFmt != (AVPixelFormat)frm->format) {
                sws_freeContext(sws);
                sws = sws_getContext(frm->width, frm->height,
                                     (AVPixelFormat)frm->format, frm->width,
                                     frm->height, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (dstData[0]) av_freep(&dstData[0]);
                if (sws && av_image_alloc(dstData, dstStride, frm->width,
                                          frm->height, AV_PIX_FMT_RGBA,
                                          64) < 0) {
                    sws_freeContext(sws);
                    sws = nullptr;
                }
                swsW = frm->width;
                swsH = frm->height;
                swsFmt = (AVPixelFormat)frm->format;
            }
            if (!sws || !dstData[0]) { av_frame_unref(frm); continue; }

            sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dstData,
                      dstStride);

            // Pixel (sample) aspect ratio: anamorphic streams must be
            // displayed wider/narrower than their pixel dimensions or they
            // look stretched. 1:1 when unknown.
            AVRational sar = av_guess_sample_aspect_ratio(
                fmt, fmt->streams[videoIdx], frm);
            float sarF = (sar.num > 0 && sar.den > 0)
                             ? (float)sar.num / (float)sar.den
                             : 1.0f;

            width_.store(frm->width);
            height_.store(frm->height);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                frameSar_ = sarF;
                // Copy rows tightly packed (stride may exceed width * 4).
                frame_.resize((size_t)frm->width * frm->height * 4);
                const int rowBytes = frm->width * 4;
                for (int y = 0; y < frm->height; ++y)
                    std::memcpy(frame_.data() + (size_t)y * rowBytes,
                                dstData[0] + (size_t)y * dstStride[0],
                                rowBytes);
                frameW_ = frm->width;
                frameH_ = frm->height;
                frameNew_ = true;
            }
            av_frame_unref(frm);

            ++fpsCount;
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<float> dt = now - fpsWindowStart;
            if (dt.count() >= 1.0f) {
                fps_.store(fpsCount / dt.count());
                fpsWindowStart = now;
                fpsCount = 0;
            }
        }
    }

    if (dstData[0]) av_freep(&dstData[0]);
    sws_freeContext(sws);
    if (swr) swr_free(&swr);
    if (afrm) av_frame_free(&afrm);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    if (adec) avcodec_free_context(&adec);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    if (state_.load() == State::Playing) state_.store(State::Ended);
}
