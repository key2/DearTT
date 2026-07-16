// DearTT — TikTok LIVE viewer.
//
//   video (FFmpeg -> OpenGL texture)  |  chat feed (ttlive-cpp)
//
// The main thread owns the window, ImGui and the GL texture. A ttlive client
// thread feeds chat + the stream URL; an FFmpeg thread feeds RGBA frames.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "misc/freetype/imgui_freetype.h"

#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

// Windows' GL/gl.h is OpenGL 1.1; this token was added in 1.2.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Silencing libprotobuf's console spam differs across major versions:
//   - protobuf <= 4.x (22.x): google::protobuf::SetLogHandler() in
//     <google/protobuf/stubs/logging.h>.
//   - protobuf >= 5.x (23+, incl. Homebrew's current build): logging moved to
//     Abseil; the stubs header is gone and severity is controlled globally via
//     absl::SetMinLogLevel / SetStderrThreshold.
#include <google/protobuf/stubs/common.h>
#if defined(GOOGLE_PROTOBUF_VERSION) && GOOGLE_PROTOBUF_VERSION < 4023000
#include <google/protobuf/stubs/logging.h>
#define DEARTT_PROTOBUF_HAS_SETLOGHANDLER 1
#else
#include <absl/log/globals.h>
#endif

#include "event_json.hpp"
#include "event_server.hpp"
#include "icon_cache.hpp"
#include "live_session.hpp"
#include "profile.hpp"
#include "stats.hpp"
#include "video_player.hpp"

#ifdef DEARTT_FACE_RECOGNITION
#include "avatar_fetch.hpp"
#include "face_tracker.hpp"
#endif

#ifdef DEARTT_STT
#include <ggml-backend.h>

#include "model_download.hpp"
#include "stt.hpp"
#endif

namespace {

void glfwErrorCb(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

bool fileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

// Load a pan-Unicode base font so chat in non-Latin scripts renders (ImGui
// 1.92 loads glyphs dynamically, no ranges needed), and merge a CJK-capable
// fallback so rare Chinese/Japanese/Korean chat doesn't turn into
// replacement boxes.
//
// The preferred base is the bundled GoNotoKurrent-Regular.ttf
// (https://github.com/satbyy/go-noto-universal, SIL OFL): 80+ merged Noto
// scripts — Latin/Greek/Cyrillic, Arabic, Hebrew, Thai, Indic, CJK
// (Unihan-IICore subset), … — identical rendering on every OS, no system
// font hunting. System fonts remain as fallback when the asset is missing.
// Note: ImGui has no shaping/bidi, so Arabic still renders as unjoined
// letterforms in logical order regardless of the font.
void loadFont(ImGuiIO& io) {
    const char* base[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        // Wine prefixes usually have no TTFs in C:\windows\Fonts; the host
        // filesystem is mapped as Z:\ — harmless (skipped) on real Windows.
        "Z:/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "Z:/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    // Only *scalable* color-emoji fonts (COLR vector layers): bitmap-strike
    // fonts (Noto Color Emoji CBDT, Apple sbix) open fine but can't rasterize
    // at arbitrary sizes with imgui_freetype, leaving replacement glyphs.
    // The bundled Twemoji (COLR) covers Linux/macOS/Wine.
    std::vector<std::string> emoji = {
#if defined(_WIN32)
        "C:/Windows/Fonts/seguiemj.ttf",  // Segoe UI Emoji (COLR)
#endif
    };
    std::string bundled = findResource("fonts/Twemoji.Mozilla.ttf");
    if (!bundled.empty()) emoji.push_back(bundled);
    const char* cjk[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/msyh.ttc",      // Microsoft YaHei (SC)
        "C:/Windows/Fonts/meiryo.ttc",    // JP
        "C:/Windows/Fonts/malgun.ttf",    // KR
        "C:/Windows/Fonts/simsun.ttc",
        // Wine fallback via the Z:\ host mapping (skipped on real Windows).
        "Z:/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "Z:/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
#endif
    };
    // The base fonts (Noto Sans / DejaVu on Linux) have no Arabic coverage, so
    // Arabic chat rendered as replacement glyphs — merge an Arabic-capable
    // font. Note: ImGui has no text shaping/bidi, so Arabic renders as
    // isolated (unjoined) letterforms in logical order — legible, but not
    // typographically correct.
    const char* arabic[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",   // Segoe UI covers Arabic
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf",
        // Wine fallback via the Z:\ host mapping (skipped on real Windows).
        "Z:/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "Z:/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf",
        "Z:/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/GeezaPro.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",  // partial Arabic
#endif
    };

    // Preferred base: the bundled pan-Unicode font.
    bool haveUniversal = false;
    std::string universal = findResource("fonts/GoNotoKurrent-Regular.ttf");
    if (!universal.empty() &&
        io.Fonts->AddFontFromFileTTF(universal.c_str(), 17.0f))
        haveUniversal = true;

    bool haveBase = haveUniversal;
    for (const char* path : base) {
        if (haveBase) break;
        if (!fileExists(path)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f)) haveBase = true;
    }
    if (!haveBase) io.Fonts->AddFontDefault();

    // GoNotoKurrent's CJK is the Unihan-IICore subset — merge a full-coverage
    // system CJK font to fill in rare ideographs (and as the only CJK source
    // when falling back to a system base font).
    ImFontConfig cfg;
    cfg.MergeMode = true;
    for (const char* path : cjk) {
        if (!fileExists(path)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg)) break;
    }

    // Arabic gap-filler — only needed when the bundled universal font (which
    // covers Arabic) is unavailable and the system base font lacks it.
    if (!haveUniversal) {
        ImFontConfig acfg;
        acfg.MergeMode = true;
        for (const char* path : arabic) {
            if (!fileExists(path)) continue;
            if (io.Fonts->AddFontFromFileTTF(path, 17.0f, &acfg)) break;
        }
    }

    // Color emoji fallback — needs the FreeType rasterizer (COLR/CBDT glyph
    // formats); stb_truetype would silently drop every emoji.
    ImFontConfig ecfg;
    ecfg.MergeMode = true;
    ecfg.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
    for (const std::string& path : emoji) {
        if (!fileExists(path.c_str())) continue;
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 17.0f, &ecfg)) break;
    }
}

// Latest-frame video texture (RGBA8).
struct VideoTexture {
    GLuint id = 0;
    int w = 0, h = 0;
    float sar = 1.0f;   ///< pixel aspect ratio; display at (w * sar) x h

    void upload(const std::vector<uint8_t>& rgba, int fw, int fh) {
        if (!id) {
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (fw != w || fh != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba.data());
            w = fw;
            h = fh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA,
                            GL_UNSIGNED_BYTE, rgba.data());
        }
    }

    void destroy() {
        if (id) glDeleteTextures(1, &id);
        id = 0;
        w = h = 0;
        sar = 1.0f;
    }
};

ImVec4 chatColor(ChatItem::Kind k) {
    switch (k) {
        case ChatItem::Kind::Gift:      return ImVec4(1.00f, 0.75f, 0.25f, 1.0f);
        case ChatItem::Kind::Join:      return ImVec4(0.55f, 0.60f, 0.70f, 1.0f);
        case ChatItem::Kind::Follow:
        case ChatItem::Kind::Share:
        case ChatItem::Kind::Subscribe: return ImVec4(0.55f, 0.85f, 1.00f, 1.0f);
        case ChatItem::Kind::System:    return ImVec4(0.95f, 0.45f, 0.55f, 1.0f);
        default:                        return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    }
}

// Wrapped chat text as a layout item. With `shadow`, TikTok-style: a dark
// drop shadow underneath plus a second slightly-offset main pass (faux bold)
// so white text stays readable directly over bright video.
void chatText(const char* s, const ImVec4& col, float size, bool shadow) {
    ImFont* font = ImGui::GetFont();
    float wrapW = ImGui::GetContentRegionAvail().x;
    if (wrapW < 40.0f) wrapW = 40.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, wrapW, s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c = ImGui::GetColorU32(col);
    if (shadow) {
        const ImU32 sc = IM_COL32(0, 0, 0, 230);
        dl->AddText(font, size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), sc, s,
                    nullptr, wrapW);
        dl->AddText(font, size, ImVec2(pos.x + 0.7f, pos.y), c, s, nullptr,
                    wrapW);  // faux bold
    }
    dl->AddText(font, size, pos, c, s, nullptr, wrapW);
    ImGui::Dummy(sz);
}

// Round avatar inline with text; grey placeholder while it downloads.
void avatarImage(unsigned tex, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex)
        dl->AddImageRounded((ImTextureID)(intptr_t)tex, p,
                            ImVec2(p.x + size, p.y + size), ImVec2(0, 0),
                            ImVec2(1, 1), IM_COL32_WHITE, size * 0.5f);
    else
        dl->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f),
                            size * 0.5f, IM_COL32(70, 70, 82, 255));
    ImGui::Dummy(ImVec2(size, size));
}

std::string fmtCount(int64_t v) {
    char buf[32];
    if (v >= 1000000)
        std::snprintf(buf, sizeof(buf), "%.1fM", v / 1e6);
    else if (v >= 10000)
        std::snprintf(buf, sizeof(buf), "%.1fk", v / 1e3);
    else
        std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return buf;
}

std::string fmtDuration(double sec) {
    int s = (int)sec;
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600,
                      (s / 60) % 60, s % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

// One key/value row of the session summary.
void statRow(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

void drawStatsPanel(StatsCollector& stats) {
    const StatsTotals t = stats.totals();
    const auto& x = stats.tMin();
    const int n = (int)x.size();

    // --- session summary ------------------------------------------------
    if (ImGui::BeginTable("summary", 4,
                          ImGuiTableFlags_SizingStretchProp)) {
        statRow("time", fmtDuration(t.elapsedSec));
        statRow("viewers", fmtCount(t.viewers) + "  (peak " +
                               fmtCount(t.peakViewers) + ")");
        statRow("diamonds", fmtCount(t.diamonds));
        // Rough creator payout: ~USD 0.005 per diamond.
        char usd[32];
        std::snprintf(usd, sizeof(usd), "~$%.2f", t.diamonds * 0.005);
        statRow("est. value", usd);
        statRow("likes", fmtCount(t.likes));
        statRow("comments", fmtCount(t.comments));
        statRow("joins", fmtCount(t.joins));
        statRow("follows", fmtCount(t.follows));
        statRow("shares", fmtCount(t.shares));
        statRow("subs", fmtCount(t.subscribes));
        statRow("chatters", fmtCount(t.uniqueChatters));
        statRow("gifters", fmtCount(t.uniqueGifters));
        ImGui::EndTable();
    }
    ImGui::Separator();

    const ImPlotFlags plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
    const ImPlotAxisFlags axFlags = ImPlotAxisFlags_AutoFit;

    // --- viewers ----------------------------------------------------------
    if (ImPlot::BeginPlot("Viewers", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            ImPlot::PlotShaded(
                "viewers", x.data(), stats.viewersSeries().data(), n, 0.0,
                ImPlotSpec(ImPlotProp_FillAlpha, 0.25f));
            ImPlot::PlotLine("viewers", x.data(),
                             stats.viewersSeries().data(), n);
        }
        ImPlot::EndPlot();
    }

    // --- gift trend (derivative of diamonds) ------------------------------
    if (ImPlot::BeginPlot("Gifts: diamonds/min", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            const ImVec4 gold(1.0f, 0.75f, 0.25f, 1.0f);
            ImPlot::PlotShaded(
                "diamonds/min", x.data(), stats.diamondsPerMin().data(), n,
                0.0,
                ImPlotSpec(ImPlotProp_FillColor, gold, ImPlotProp_FillAlpha,
                           0.3f));
            ImPlot::PlotLine("diamonds/min", x.data(),
                             stats.diamondsPerMin().data(), n,
                             ImPlotSpec(ImPlotProp_LineColor, gold));
        }
        ImPlot::EndPlot();
    }

    // --- likes -------------------------------------------------------------
    if (ImPlot::BeginPlot("Likes/min", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            const ImVec4 pink(1.0f, 0.45f, 0.60f, 1.0f);
            ImPlot::PlotShaded(
                "likes/min", x.data(), stats.likesPerMin().data(), n, 0.0,
                ImPlotSpec(ImPlotProp_FillColor, pink, ImPlotProp_FillAlpha,
                           0.3f));
            ImPlot::PlotLine("likes/min", x.data(),
                             stats.likesPerMin().data(), n,
                             ImPlotSpec(ImPlotProp_LineColor, pink));
        }
        ImPlot::EndPlot();
    }

    // --- activity ----------------------------------------------------------
    if (ImPlot::BeginPlot("Comments/min", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            ImPlot::PlotLine("comments", x.data(),
                             stats.commentsPerMin().data(), n);
        }
        ImPlot::EndPlot();
    }

    if (n == 0)
        ImGui::TextDisabled("waiting for events...");
}

// Gifts column: Top gifters (1/3) over the per-gift list (2/3), both
// scrollable. The gift list is ordered by units sent (count), not diamonds.
void drawGiftsPanel(StatsCollector& stats, IconCache& icons,
                    IconCache& avatars) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float headerH = ImGui::GetFrameHeightWithSpacing();

    ImGui::SeparatorText("Top gifters");
    ImGui::BeginChild("topgifters",
                      ImVec2(0, avail.y * (1.0f / 3.0f) - headerH));
    {
        auto gifters = stats.topGifters(100);
        int rank = 1;
        for (const auto& g : gifters) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%d.", rank++);
            ImGui::SameLine();
            avatarImage(avatars.texture(g.id), 18.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted(g.name.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
            ImGui::TextDisabled("%s dia", fmtCount(g.diamonds).c_str());
        }
        if (gifters.empty()) ImGui::TextDisabled("no gifts yet");
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Gifts (by count)");
    ImGui::BeginChild("giftlist", ImVec2(0, 0));
    {
        auto gifts = stats.giftStats(/*byCount=*/true);
        int64_t totalCount = 0;
        for (const auto& g : gifts) totalCount += g.count;
        if (!gifts.empty() &&
            ImGui::BeginTable(
                "gifts", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed,
                                    26.0f);
            ImGui::TableSetupColumn("gift", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("dia", ImGuiTableColumnFlags_WidthFixed,
                                    52.0f);
            ImGui::TableHeadersRow();
            for (const auto& g : gifts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (unsigned tex = icons.texture(g.id))
                    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(22, 22));
                else
                    ImGui::Dummy(ImVec2(22, 22));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g.name.empty() ? "?" : g.name.c_str());
                if (g.price > 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%d diamonds each", g.price);
                ImGui::TableNextColumn();
                ImGui::Text("%s", fmtCount(g.count).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", totalCount > 0
                                          ? 100.0 * g.count / totalCount
                                          : 0.0);
                ImGui::TableNextColumn();
                ImGui::Text("%s", fmtCount(g.diamonds).c_str());
            }
            ImGui::EndTable();
        }
        if (gifts.empty()) ImGui::TextDisabled("no gifts yet");
    }
    ImGui::EndChild();
}

const char* stateText(LiveSession::State s) {
    switch (s) {
        case LiveSession::State::Idle:         return "idle";
        case LiveSession::State::Connecting:   return "connecting...";
        case LiveSession::State::Connected:    return "LIVE";
        case LiveSession::State::Disconnected: return "disconnected";
        case LiveSession::State::Error:        return "error";
    }
    return "?";
}

#ifdef DEARTT_FACE_RECOGNITION
// Files dropped onto the window (GLFW drop callback, main thread during
// glfwPollEvents); consumed by the avatar picture picker when open.
std::vector<std::string> g_droppedFiles;
void dropCb(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; i++) g_droppedFiles.emplace_back(paths[i]);
}

// Small GL-texture cache for display-only profile pictures, keyed by person.
struct AvatarTextures {
    struct Entry { std::string path; unsigned tex = 0; };
    std::map<std::string, Entry> map_;

    unsigned get(const std::string& person, const std::string& path) {
        if (path.empty()) return 0;
        auto it = map_.find(person);
        if (it != map_.end() && it->second.path == path) return it->second.tex;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!decodeImageFileRGBA(path, rgba, w, h, 96)) return 0;
        unsigned tex = (it != map_.end()) ? it->second.tex : 0u;
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());
        map_[person] = {path, tex};
        return tex;
    }
    void invalidate(const std::string& person) {
        auto it = map_.find(person);
        if (it != map_.end()) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            map_.erase(it);
        }
    }
    void shutdown() {
        for (auto& [k, e] : map_)
            if (e.tex) glDeleteTextures(1, &e.tex);
        map_.clear();
    }
};
#endif

}  // namespace

int main(int argc, char** argv) {
    // The reflective webview JSON decode (event_json.cpp) parses every
    // Webcast message type. TikTok's schema drifts and some auto-generated
    // fields are mislabeled string-vs-bytes, which makes libprotobuf spam
    // "invalid UTF-8" errors to the console — drop its logging entirely
    // (failed parses already fall back to raw_base64 gracefully).
#if defined(DEARTT_PROTOBUF_HAS_SETLOGHANDLER)
    google::protobuf::SetLogHandler(nullptr);
#else
    // protobuf 5.x+: raise the Abseil log threshold above every level protobuf
    // emits so its "invalid UTF-8" warnings never reach stderr.
    absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfinity);
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
#endif

#ifdef DEARTT_STT
    // Bootstrap ggml's backend registry (including its Vulkan instance and
    // device enumeration) BEFORE any OpenGL context exists. Under Wine, the
    // host Vulkan loader fails to initialize once a GLX context is live
    // (winevulkan asserts in loader.c and the worker threads die, leaving
    // STT stuck at "loading model" and the face tracker silent). Doing the
    // first touch here, on the main thread, is fast and harmless on real
    // Windows/Linux.
    ggml_backend_dev_count();
#endif

    glfwSetErrorCallback(glfwErrorCb);
#if defined(GLFW_PLATFORM) && !defined(_WIN32) && !defined(__APPLE__)
    // Allow forcing the windowing backend on Linux: GLFW_PLATFORM=x11|wayland
    if (const char* plat = std::getenv("GLFW_PLATFORM")) {
        if (!std::strcmp(plat, "x11"))
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        else if (!std::strcmp(plat, "wayland"))
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    }
#endif
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window =
        glfwCreateWindow(1280, 760, "DearTT - TikTok LIVE viewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync
#ifdef DEARTT_FACE_RECOGNITION
    glfwSetDropCallback(window, dropCb);  // profile-picture drag & drop
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    loadFont(io);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // Event forwarder: website on http://localhost:<port>/, every TikTok
    // event as JSON on ws://localhost:<port>/ws. Declared before the session
    // so it outlives the client thread that broadcasts into it.
    EventServer eventServer;
    int wsPort = 8080;
    if (const char* p = std::getenv("DEARTT_PORT")) {
        int v = std::atoi(p);
        if (v > 0 && v < 65536) wsPort = v;
    }
    eventServer.start(wsPort, findWebDir());

    LiveSession session;
    VideoPlayer player;
    VideoTexture videoTex;
    StatsCollector stats;
    IconCache giftIcons;
    IconCache avatars;

#ifdef DEARTT_FACE_RECOGNITION
    FaceTracker faceTracker;
    {
        std::string mdir = findResource("models");
        if (!mdir.empty()) faceTracker.start(mdir);
    }
#endif

#ifdef DEARTT_STT
    SttEngine stt;
    ModelDownloader sttDownload;
    bool sttEnabled = false;
    bool sttAvailable = false;
    std::string sttModelPath;
    {
        // Resolve the Voxtral GGUF: $DEARTT_VOXTRAL_MODEL, else the first
        // .gguf under models/voxtral/.
        if (const char* m = std::getenv("DEARTT_VOXTRAL_MODEL"))
            sttModelPath = m;
        if (sttModelPath.empty()) {
            std::string dir = findResource("models/voxtral");
            std::error_code ec;
            if (!dir.empty())
                for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                    if (e.path().extension() == ".gguf") {
                        sttModelPath = e.path().string();
                        break;
                    }
        }
        if (!sttModelPath.empty()) {
            sttAvailable = true;
            stt.start(sttModelPath);
        } else if (!std::getenv("DEARTT_NO_MODEL_DOWNLOAD")) {
            // First run: the model is not shipped in the distributable
            // (~2.7 GB). Fetch it next to the executable in the background;
            // STT lights up when the download finishes.
            const char* u = std::getenv("DEARTT_VOXTRAL_URL");
            std::string url =
                u ? u
                  : "https://huggingface.co/andrijdavid/"
                    "Voxtral-Mini-4B-Realtime-2602-GGUF/resolve/main/"
                    "Q4_K_M.gguf";
            sttModelPath = (std::filesystem::path(resourceRootDir()) /
                            "models" / "voxtral" / "Q4_K_M.gguf")
                               .string();
            sttDownload.start(url, sttModelPath);
        }
        // Feed the decoded audio to STT (resampled to 16 kHz mono inside).
        // Always tapped: pushAudio is a no-op until the engine is ready, and
        // the model may only become available after the download.
        player.setAudioTap([&stt](const float* pcm, int frames, int ch,
                                  int rate) {
            stt.pushAudio(pcm, frames, ch, rate);
        });
    }
#endif

    session.setEventSink(
        [&eventServer, &stats, &avatars](const ttlive::Event& e) {
            stats.record(e);
            // Fetch the avatar of anyone we might display (chat, top
            // gifters); IconCache dedupes and caps by itself.
            avatars.request(e.user.id, e.user.avatar_url);
            eventServer.broadcast(eventToJson(e));
        });

    // "GDI Generic" (Windows) or "llvmpipe" (Linux) here means software
    // rendering — the whole UI will be slow regardless of decoding.
    const char* glRenderer = (const char*)glGetString(GL_RENDERER);
    std::string renderer = glRenderer ? glRenderer : "?";

    // --- profiles: a profile pairs a stream with the people expected on it.
    std::vector<std::string> profileList = profiles::list();
    int profileIdx = -1;
    Profile curProfile;
    bool profileLoaded = false;
    bool showProfileMgr = false;
    char newProfName[128] = "";
    char newProfStream[128] = "";
    char newPersonName[128] = "";
    int assignTrackId = 0;          // face clicked for naming (for highlight)
    char assignNewName[128] = "";
    std::vector<float> assignEmb;   // embedding snapshotted at click time
    float assignArea = 0.0f;
#ifdef DEARTT_FACE_RECOGNITION
    AvatarFetcher avatarFetcher;
    AvatarTextures avatarTex;
    std::string avatarPicTarget;    // person the picture popup is editing
    std::string avatarDropTarget;   // person to receive a dropped image now
    char avatarTT[128] = "";        // TikTok @username input
    char avatarURL[256] = "";       // image URL input
#endif

    auto selectProfile = [&](int idx) {
        profileIdx = idx;
        profileLoaded = (idx >= 0 && idx < (int)profileList.size() &&
                         profiles::load(profileList[idx], curProfile));
#ifdef DEARTT_FACE_RECOGNITION
        if (profileLoaded) faceTracker.selectProfile(curProfile.dir);
#endif
    };

    // Optional dev shortcut: `deartt <username>` still connects directly.
    char userBuf[128] = "";
    if (argc > 1) {
        std::snprintf(userBuf, sizeof(userBuf), "%s",
                      argv[1][0] == '@' ? argv[1] + 1 : argv[1]);
        session.start(userBuf);
    }

    std::vector<ChatItem> chat;
    bool chatAutoScroll = true;
    bool chatOnly = false;   // hide joins/gifts/system, show comments only
    bool showStats = true;
    enum { kChatRight = 0, kChatOverlay = 1, kChatOff = 2 };
    int chatMode = kChatRight;
    size_t chatAdded = 0;    // items appended this frame (for autoscroll)
    bool playerStarted = false;
    bool noStreamUrl = false;   // connected, but room gave us no video URLs
    std::vector<uint8_t> frameRgba;
    ttlive::StreamInfo streamInfo;   // qualities of the connected room
    std::string currentQuality;     // active quality key (e.g. "HD1")

    // Chat feed items + autoscroll; rendered either into the right-side pane
    // (shadow=false) or over the video (shadow=true, TikTok-style shadowed
    // bold text so it stays readable without a background panel).
    auto drawChatFeed = [&](bool shadow) {
        const float chatAvatarSz = 36.0f;                    // 2x
        const float chatFontSz = ImGui::GetFontSize() * 1.5f;
        const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
        const ImVec4 nameBlue(0.55f, 0.75f, 1.0f, 1.0f);
        for (const ChatItem& item : chat) {
            if (chatOnly && item.kind != ChatItem::Kind::Comment) continue;
            if (item.kind == ChatItem::Kind::Comment) {
                // [avatar]  Name
                //           message  (aligned with the name, not the icon)
                avatarImage(avatars.texture(item.userId), chatAvatarSz);
                ImGui::SameLine();
                ImGui::BeginGroup();  // lines below start at the name's x
                chatText(item.author.c_str(), nameBlue, chatFontSz, shadow);
                chatText(item.text.c_str(), white, chatFontSz, shadow);
                ImGui::EndGroup();
            } else if (item.author.empty()) {
                chatText(item.text.c_str(), chatColor(item.kind),
                         ImGui::GetFontSize(), shadow);
            } else {
                if (item.userId) {
                    avatarImage(avatars.texture(item.userId), chatAvatarSz);
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                std::string line = item.author + " " + item.text;
                chatText(line.c_str(), chatColor(item.kind), chatFontSz,
                         shadow);
                ImGui::EndGroup();
            }
            ImGui::Spacing();
        }
        if (chatAutoScroll && chatAdded > 0) ImGui::SetScrollHereY(1.0f);
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // --- session -> player handoff -------------------------------------
        if (session.state() == LiveSession::State::Connected && !playerStarted &&
            !noStreamUrl) {
            streamInfo = session.streamInfo();
            // Start with the stream's default quality (or whatever exists).
            currentQuality.clear();
            // Prefer 720p (HD1) when the room offers it; fall back to the
            // stream's default quality otherwise.
            std::string url = streamUrlFor(streamInfo, "HD1");
            if (url.empty()) url = streamUrlFor(streamInfo);
            for (const auto& q : streamInfo.qualities)
                if (!q.hls_url.empty() || !q.flv_url.empty()) {
                    if (url == q.hls_url || url == q.flv_url) {
                        currentQuality = q.quality;
                        break;
                    }
                }
            if (!url.empty()) {
                player.open(url);
                playerStarted = true;
            } else {
                // Room info returned no URLs at all: typically an age-/
                // audience-restricted LIVE, which TikTok only serves to
                // logged-in sessions (status_code 4003110).
                noStreamUrl = true;
            }
            // Kick off gift icon downloads (names/prices/icons per gift).
            for (const auto& g : session.giftList())
                giftIcons.request(g.id, g.icon_url);
        }
        if (session.state() != LiveSession::State::Connected &&
            session.state() != LiveSession::State::Connecting) {
            if (playerStarted) {
                player.close();
                playerStarted = false;
            }
            noStreamUrl = false;
        }

        // --- pull chat + video frame ---------------------------------------
        chatAdded = session.drainChat(chat);
        if (chat.size() > 5000)
            chat.erase(chat.begin(), chat.begin() + (chat.size() - 5000));

        int fw = 0, fh = 0;
        float fsar = 1.0f;
        if (player.takeFrame(frameRgba, fw, fh, fsar)) {
            videoTex.upload(frameRgba, fw, fh);
            videoTex.sar = fsar;
#ifdef DEARTT_FACE_RECOGNITION
            if (faceTracker.running())
                faceTracker.submitFrame(frameRgba.data(), fw, fh);
#endif
        }

        stats.sample();      // 1 Hz time series point (throttled internally)
        giftIcons.upload();  // decoded icons -> GL textures
        avatars.upload();

#ifdef DEARTT_STT
        // First-run model download finished -> bring STT up.
        if (!sttAvailable && sttDownload.finished()) {
            sttAvailable = true;
            stt.start(sttModelPath);
        }
#endif

#ifdef DEARTT_FACE_RECOGNITION
        // Apply finished profile-picture downloads.
        for (const auto& r : avatarFetcher.poll())
            if (r.ok) {
                faceTracker.setAvatar(r.person, r.path);
                avatarTex.invalidate(r.person);
            }
        avatarDropTarget.clear();  // set again this frame if the picker is open
#endif

        // --- UI --------------------------------------------------------------
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);

        // Top bar: profile picker + connect controls + status.
        {
            bool busy = session.state() == LiveSession::State::Connecting ||
                        session.state() == LiveSession::State::Connected;

            // Profile combo.
            ImGui::SetNextItemWidth(180);
            const char* preview = profileLoaded ? curProfile.name.c_str()
                                                 : "(select profile)";
            if (ImGui::BeginCombo("##profile", preview)) {
                for (int i = 0; i < (int)profileList.size(); i++) {
                    bool sel = (i == profileIdx);
                    if (ImGui::Selectable(profileList[i].c_str(), sel) && !busy)
                        selectProfile(i);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("New")) {
                newProfName[0] = newProfStream[0] = '\0';
                ImGui::OpenPopup("New profile");
            }
            ImGui::SameLine();
            if (ImGui::Button("Manage") && profileLoaded)
                showProfileMgr = true;

            // New-profile modal.
            if (ImGui::BeginPopupModal("New profile", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputTextWithHint("name", "e.g. FridayShow",
                                         newProfName, sizeof(newProfName));
                ImGui::InputTextWithHint("stream", "@username", newProfStream,
                                         sizeof(newProfStream));
                if (ImGui::Button("Create") && newProfName[0]) {
                    if (profiles::create(newProfName, newProfStream)) {
                        profileList = profiles::list();
                        for (int i = 0; i < (int)profileList.size(); i++)
                            if (profileList[i] == std::string(newProfName)) {
                                selectProfile(i);
                                break;
                            }
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            if (!busy) {
                bool canConnect = profileLoaded && !curProfile.stream.empty();
                if (!canConnect) ImGui::BeginDisabled();
                if (ImGui::Button("Connect") && canConnect) {
                    chat.clear();
                    videoTex.destroy();
                    playerStarted = false;
                    session.start(curProfile.stream);
                }
                if (!canConnect) ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Disconnect")) {
                    session.stop();
                    player.close();
                    playerStarted = false;
                }
            }

            ImGui::SameLine();
            LiveSession::State st = session.state();
            ImVec4 stCol = st == LiveSession::State::Connected
                               ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f)
                           : st == LiveSession::State::Error
                               ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                               : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            ImGui::TextColored(stCol, "[%s]", stateText(st));
            if (st == LiveSession::State::Error) {
                ImGui::SameLine();
                ImGui::TextWrapped("%s", session.error().c_str());
            }
            if (st == LiveSession::State::Connected) {
                ImGui::SameLine();
                ImGui::Text("@%s   %lld viewers   %lld likes",
                            session.roomTitleUser().c_str(),
                            (long long)session.viewerCount(),
                            (long long)session.totalLikes());

                // Audio: mute toggle + volume slider.
                if (playerStarted) {
                    AudioOutput& au = player.audio();
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 320);
                    bool muted = au.muted();
                    if (ImGui::Checkbox("mute", &muted)) au.setMuted(muted);
                    ImGui::SameLine();
                    float vol = au.volume();
                    ImGui::SetNextItemWidth(120);
                    ImGui::BeginDisabled(muted);
                    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "vol %.2f"))
                        au.setVolume(vol);
                    ImGui::EndDisabled();
                }

                // Quality switch (only qualities that actually have a URL).
                if (playerStarted && streamInfo.qualities.size() > 1) {
                    const ttlive::StreamQuality* cur = nullptr;
                    for (const auto& q : streamInfo.qualities)
                        if (q.quality == currentQuality) cur = &q;
                    std::string curLabel =
                        cur ? (cur->label.empty() ? cur->quality : cur->label)
                            : "auto";
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
                    ImGui::SetNextItemWidth(88);
                    if (ImGui::BeginCombo("##quality", curLabel.c_str())) {
                        for (const auto& q : streamInfo.qualities) {
                            if (q.hls_url.empty() && q.flv_url.empty())
                                continue;
                            std::string label =
                                q.label.empty() ? q.quality : q.label;
                            bool selected = q.quality == currentQuality;
                            if (ImGui::Selectable(label.c_str(), selected) &&
                                !selected) {
                                std::string url =
                                    streamUrlFor(streamInfo, q.quality);
                                if (!url.empty()) {
                                    currentQuality = q.quality;
                                    player.open(url);  // open() closes first
                                }
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }
        }
        if (eventServer.running())
            ImGui::TextDisabled(
                "web http://localhost:%d  ·  ws://localhost:%d/ws  ·  %d "
                "client%s",
                eventServer.port(), eventServer.port(),
                eventServer.clientCount(),
                eventServer.clientCount() == 1 ? "" : "s");
        else
            ImGui::TextDisabled("web server not running (%s)",
                                eventServer.error().c_str());
        // In overlay mode the chat pane header doesn't exist, so its two
        // toggles move up here instead of floating over the video.
        float ctrlsW = 356.0f + (chatMode == kChatOverlay ? 208.0f : 0.0f);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ctrlsW);
        ImGui::TextDisabled("chat:");
        ImGui::SameLine();
        ImGui::RadioButton("right", &chatMode, kChatRight);
        ImGui::SameLine();
        ImGui::RadioButton("overlay", &chatMode, kChatOverlay);
        ImGui::SameLine();
        ImGui::RadioButton("off", &chatMode, kChatOff);
        if (chatMode == kChatOverlay) {
            ImGui::SameLine();
            ImGui::Checkbox("chat only", &chatOnly);
            ImGui::SameLine();
            ImGui::Checkbox("autoscroll", &chatAutoScroll);
        }
        ImGui::SameLine();
        ImGui::Checkbox("stats", &showStats);
#ifdef DEARTT_STT
        ImGui::SameLine();
        if (!sttAvailable) ImGui::BeginDisabled();
        if (ImGui::Checkbox("STT", &sttEnabled)) stt.setEnabled(sttEnabled);
        if (!sttAvailable) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            if (sttAvailable)
                ImGui::SetTooltip(
                    "Speech-to-text (Voxtral): transcribe the stream audio\n"
                    "into the panel next to the video.");
            else if (sttDownload.active())
                ImGui::SetTooltip(
                    "Downloading the Voxtral model — STT unlocks when the\n"
                    "download finishes.");
            else if (!sttDownload.error().empty())
                ImGui::SetTooltip("Model download failed: %s\n"
                                  "Restart the app to retry (it resumes).",
                                  sttDownload.error().c_str());
            else
                ImGui::SetTooltip(
                    "No Voxtral model found (models/voxtral/*.gguf or\n"
                    "$DEARTT_VOXTRAL_MODEL).");
        }
#endif
#ifdef DEARTT_FACE_RECOGNITION
        if (faceTracker.running()) {
            // Identity smoothing window (temporal vote + coast duration).
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            float win = faceTracker.identityWindow();
            if (ImGui::SliderFloat("##idwin", &win, 1.0f, 10.0f, "id %.0fs"))
                faceTracker.setIdentityWindow(win);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Identity window: names are the majority vote over the\n"
                    "last N seconds, and a face that briefly turns away is\n"
                    "kept for this long. Longer = steadier, slower to switch.");
        }
#endif
        ImGui::Separator();

        // Split: video (left, stretch) | STT | gifts | stats | chat (fixed
        // panes; chat only in "right" mode — "overlay" draws over the video).
        const float chatWidth = chatMode == kChatRight ? 340.0f : 0.0f;
        const float giftsWidth = showStats ? 300.0f : 0.0f;
        const float statsWidth = showStats ? 330.0f : 0.0f;
        float sttWidth = 0.0f;
#ifdef DEARTT_STT
        if (sttEnabled) sttWidth = 320.0f;
#endif
        int sidePanes = (showStats ? 2 : 0) + (chatMode == kChatRight ? 1 : 0) +
                        (sttWidth > 0 ? 1 : 0);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float videoWidth = avail.x - chatWidth - giftsWidth - statsWidth -
                           sttWidth - 8.0f * (float)sidePanes;

        ImGui::BeginChild("video", ImVec2(videoWidth, 0),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        {
            ImVec2 area = ImGui::GetContentRegionAvail();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p0, ImVec2(p0.x + area.x, p0.y + area.y),
                              IM_COL32(8, 8, 10, 255));

            // Rectangle actually covered by the video after aspect-fit
            // (the child is letterboxed); the chat overlay anchors to it.
            ImVec2 vidPos = p0, vidSize = area;

            if (videoTex.id && videoTex.w > 0) {
                // Aspect-fit on the DISPLAY size: anamorphic streams have
                // non-square pixels (sar != 1), so the presented width is
                // w * sar — using raw pixel dimensions would distort them.
                float dispW = videoTex.w * videoTex.sar;
                float scale = std::min(area.x / dispW, area.y / videoTex.h);
                ImVec2 sz(dispW * scale, videoTex.h * scale);
                ImVec2 off(p0.x + (area.x - sz.x) * 0.5f,
                           p0.y + (area.y - sz.y) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)videoTex.id, off,
                             ImVec2(off.x + sz.x, off.y + sz.y));
                vidPos = off;
                vidSize = sz;

#ifdef DEARTT_FACE_RECOGNITION
                // Face bounding boxes + identity labels; click a face to name.
                if (faceTracker.running()) {
                    // Map from frame pixels to display pixels.
                    float fx = sz.x / (videoTex.w * videoTex.sar);
                    float fy = sz.y / (float)videoTex.h;
                    ImVec2 mouse = ImGui::GetMousePos();
                    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                   ImGui::IsWindowHovered();
                    auto trackedFaces = faceTracker.faces();
                    // name -> display-picture path, for the label thumbnails.
                    std::map<std::string, std::string> avByName;
                    std::map<std::string, int> idxByName;  // roster position
                    {
                        int i = 0;
                        for (const auto& p : faceTracker.roster()) {
                            if (!p.avatar.empty()) avByName[p.name] = p.avatar;
                            idxByName[p.name] = ++i;  // 1..N, stable per person
                        }
                    }
                    for (const auto& tf : trackedFaces) {
                        ImVec2 bmin(off.x + tf.box.x * fx,
                                    off.y + tf.box.y * fy);
                        ImVec2 bmax(bmin.x + tf.box.w * fx,
                                    bmin.y + tf.box.h * fy);
                        // Green = named, blue = unknown; dimmed while coasting.
                        int a = tf.coasting ? 110 : 220;
                        ImU32 col = tf.name.empty()
                                        ? IM_COL32(100, 200, 255, a)
                                        : IM_COL32(80, 255, 120, a);
                        dl->AddRect(bmin, bmax, col, 4.0f, 0, 2.0f);

                        // Click inside the box -> snapshot THIS face's
                        // embedding now, then open the naming popup. The
                        // assignment later uses this fixed snapshot, so it
                        // can't drift to another face while the popup is open.
                        if (clicked && mouse.x >= bmin.x && mouse.x <= bmax.x &&
                            mouse.y >= bmin.y && mouse.y <= bmax.y &&
                            !tf.embedding.empty()) {
                            assignTrackId = tf.trackId;
                            assignEmb = tf.embedding;
                            assignArea = tf.box.w * tf.box.h;
                            assignNewName[0] = '\0';
                            ImGui::OpenPopup("AssignFace");
                        }

                        // Highlight the face currently being named.
                        if (tf.trackId == assignTrackId &&
                            ImGui::IsPopupOpen("AssignFace"))
                            dl->AddRect(ImVec2(bmin.x - 2, bmin.y - 2),
                                        ImVec2(bmax.x + 2, bmax.y + 2),
                                        IM_COL32(255, 230, 90, 255), 5.0f, 0,
                                        3.0f);

                        // Badge anchored to the roster, not the track: a known
                        // person shows their fixed number (1..N, never more
                        // than the people in the profile); an unknown face
                        // shows "?" (click it to name). No runaway counter.
                        char idxBuf[8];
                        auto ii = idxByName.find(tf.name);
                        if (!tf.name.empty() && ii != idxByName.end())
                            std::snprintf(idxBuf, sizeof(idxBuf), "#%d",
                                          ii->second);
                        else
                            std::snprintf(idxBuf, sizeof(idxBuf), "?");
                        ImVec2 is = ImGui::CalcTextSize(idxBuf);
                        dl->AddRectFilled(
                            bmin,
                            ImVec2(bmin.x + is.x + 6, bmin.y + is.y + 4),
                            IM_COL32(0, 0, 0, 200), 3.0f);
                        dl->AddText(ImVec2(bmin.x + 3, bmin.y + 2),
                                    IM_COL32(255, 255, 255, 255), idxBuf);

                        // Identity label above the box: name + match quality
                        // (cosine similarity, not vote agreement — a weak
                        // match now reads low instead of a misleading 100%).
                        if (!tf.name.empty()) {
                            char label[128];
                            std::snprintf(label, sizeof(label), "%s (%.0f%%)",
                                          tf.name.c_str(),
                                          tf.similarity * 100.0f);
                            ImVec2 ts = ImGui::CalcTextSize(label);
                            // Optional display-picture thumbnail before the name.
                            unsigned av = 0;
                            auto ai = avByName.find(tf.name);
                            if (ai != avByName.end())
                                av = avatarTex.get(tf.name, ai->second);
                            float avSz = av ? ts.y + 4 : 0.0f;
                            ImVec2 lp(bmin.x, bmin.y - ts.y - 4);
                            dl->AddRectFilled(
                                ImVec2(lp.x - 2, lp.y - 1),
                                ImVec2(lp.x + avSz + ts.x + 4, lp.y + ts.y + 2),
                                IM_COL32(0, 0, 0, 180), 3.0f);
                            if (av) {
                                dl->AddImageRounded(
                                    (ImTextureID)(intptr_t)av,
                                    ImVec2(lp.x, lp.y),
                                    ImVec2(lp.x + avSz - 2, lp.y + avSz - 2),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                                    (avSz - 2) * 0.5f);
                            }
                            dl->AddText(ImVec2(lp.x + avSz, lp.y), col, label);
                        }
                    }

                    // Naming popup: assigns the embedding snapshotted at
                    // click (assignEmb), so the target face is locked in.
                    if (ImGui::BeginPopup("AssignFace")) {
                        ImGui::TextDisabled("Assign this face to:");
                        ImGui::Separator();
                        auto roster = faceTracker.roster();
                        for (const auto& p : roster) {
                            std::string lbl = p.name + (p.hasFace ? "  *" : "");
                            if (ImGui::Selectable(lbl.c_str())) {
                                faceTracker.assignEmbedding(p.name, assignEmb,
                                                            assignArea);
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        if (roster.empty())
                            ImGui::TextDisabled("no people yet");
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(160);
                        bool go = ImGui::InputTextWithHint(
                            "##newassign", "new name", assignNewName,
                            sizeof(assignNewName),
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        ImGui::SameLine();
                        if ((ImGui::Button("Add") || go) && assignNewName[0]) {
                            faceTracker.addPerson(assignNewName);
                            faceTracker.assignEmbedding(assignNewName, assignEmb,
                                                        assignArea);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                }
#endif

                // Stats overlay: decode fps vs UI fps + GL renderer + STT
                // compute backend.
                char sarTxt[32] = "";
                if (videoTex.sar < 0.999f || videoTex.sar > 1.001f)
                    std::snprintf(sarTxt, sizeof(sarTxt), " (sar %.3f)",
                                  videoTex.sar);
                char sttTxt[48] = "";
#ifdef DEARTT_STT
                if (stt.ready())
                    std::snprintf(sttTxt, sizeof(sttTxt), " | stt %s",
                                  stt.backend().c_str());
#endif
                char stats[256];
                std::snprintf(stats, sizeof(stats),
                              "%dx%d%s @ %.1f fps | ui %.0f fps | %s%s",
                              videoTex.w, videoTex.h, sarTxt, player.fps(),
                              io.Framerate, renderer.c_str(), sttTxt);
                ImVec2 tp(p0.x + 6, p0.y + 4);
                dl->AddRectFilled(
                    ImVec2(tp.x - 3, tp.y - 2),
                    ImVec2(tp.x + ImGui::CalcTextSize(stats).x + 3,
                           tp.y + ImGui::GetTextLineHeight() + 2),
                    IM_COL32(0, 0, 0, 140));
                dl->AddText(tp, IM_COL32(180, 220, 180, 255), stats);
            } else {
                const char* msg = "no video";
                switch (player.state()) {
                    case VideoPlayer::State::Opening: msg = "opening stream..."; break;
                    case VideoPlayer::State::Playing: msg = "buffering..."; break;
                    case VideoPlayer::State::Ended:   msg = "stream ended"; break;
                    case VideoPlayer::State::Error:   msg = "video error"; break;
                    default: break;
                }
                if (noStreamUrl)
                    msg = "TikTok returned no stream URL for this room.\n"
                          "This LIVE is likely age-restricted, and TikTok\n"
                          "only serves its video to logged-in sessions.\n"
                          "\n"
                          "Restart with DEARTT_COOKIES=\"sessionid=<value>\"\n"
                          "(cookie from a logged-in tiktok.com browser tab).";
                ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText(ImVec2(std::max(p0.x + 12.0f,
                                            p0.x + (area.x - ts.x) * 0.5f),
                                   p0.y + (area.y - ts.y) * 0.5f),
                            IM_COL32(160, 160, 170, 255), msg);
                if (player.state() == VideoPlayer::State::Error) {
                    std::string err = player.error();
                    ImVec2 es = ImGui::CalcTextSize(err.c_str());
                    dl->AddText(ImVec2(p0.x + (area.x - es.x) * 0.5f,
                                       p0.y + (area.y - ts.y) * 0.5f + 22),
                                IM_COL32(220, 90, 90, 255), err.c_str());
                }
            }

            // Chat overlay: a column over the bottom 2/5 of the VIDEO
            // rectangle (not the letterboxed child), anchored to the
            // video's left edge with a small margin. Fully transparent.
            if (chatMode == kChatOverlay) {
                const float margin = 14.0f;
                float oh = vidSize.y * 0.4f - margin;
                float ow = vidSize.x - 2.0f * margin;  // full video width
                ImVec2 op(vidPos.x + margin,
                          vidPos.y + vidSize.y - oh - margin);

                ImGui::SetCursorScreenPos(op);
                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::BeginChild("chatoverlay", ImVec2(ow, oh),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar);
                drawChatFeed(true);  // shadowed text over the video
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

#ifdef DEARTT_STT
        // Speech-to-text column, right next to the video.
        if (sttWidth > 0) {
            ImGui::SameLine();
            ImGui::BeginChild("sttpane", ImVec2(sttWidth, 0),
                              ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Speech-to-text");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44);
            if (ImGui::SmallButton("clear")) stt.clearTranscript();
            ImGui::TextDisabled("%s", stt.status().c_str());
            ImGui::Separator();
            ImGui::BeginChild("sttlog", ImVec2(0, 0), ImGuiChildFlags_None);
            std::string tr = stt.transcript();
            if (tr.empty())
                ImGui::TextDisabled(stt.ready() ? "listening..."
                                                : "loading model...");
            else {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(tr.c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
                    ImGui::SetScrollHereY(1.0f);  // autoscroll when at bottom
            }
            ImGui::EndChild();
            ImGui::EndChild();
        }
#endif

        if (showStats) {
            ImGui::SameLine();
            ImGui::BeginChild("giftspane", ImVec2(giftsWidth, 0),
                              ImGuiChildFlags_None);
            drawGiftsPanel(stats, giftIcons, avatars);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("statspane", ImVec2(statsWidth, 0),
                              ImGuiChildFlags_None);
            drawStatsPanel(stats);
            ImGui::EndChild();
        }

        if (chatMode == kChatRight) {
            ImGui::SameLine();
            ImGui::BeginChild("chatpane", ImVec2(0, 0), ImGuiChildFlags_None);
            ImGui::TextUnformatted("Chat");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
            ImGui::Checkbox("chat only", &chatOnly);
            ImGui::SameLine();
            ImGui::Checkbox("autoscroll", &chatAutoScroll);
            ImGui::Separator();

            ImGui::BeginChild("chatlog", ImVec2(0, 0),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            drawChatFeed(false);
            ImGui::EndChild();
            ImGui::EndChild();
        }

        ImGui::End();

        // --- profile manager: stream + roster (add / delete / reset) --------
        if (showProfileMgr && profileLoaded) {
            ImGui::SetNextWindowSize(ImVec2(380, 440), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Manage profile", &showProfileMgr)) {
                ImGui::Text("Profile: %s", curProfile.name.c_str());

                // Editable stream.
                static char streamBuf[128];
                std::snprintf(streamBuf, sizeof(streamBuf), "%s",
                              curProfile.stream.c_str());
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputTextWithHint(
                        "##stream", "@username", streamBuf, sizeof(streamBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    profiles::setStream(curProfile.name, streamBuf);
                    profiles::load(curProfile.name, curProfile);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(stream)");
                ImGui::Separator();

                // Add a person to the roster.
                ImGui::SetNextItemWidth(200);
                bool addP = ImGui::InputTextWithHint(
                    "##person", "new person name", newPersonName,
                    sizeof(newPersonName), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Add") || addP) && newPersonName[0]) {
#ifdef DEARTT_FACE_RECOGNITION
                    faceTracker.addPerson(newPersonName);
#endif
                    newPersonName[0] = '\0';
                }
                ImGui::TextDisabled(
                    "Click a face in the video to name it. * = face captured.");

                // Roster list with per-person Reset / Delete.
                ImGui::BeginChild("people", ImVec2(0, 0), true);
#ifdef DEARTT_FACE_RECOGNITION
                auto roster = faceTracker.roster();
                bool openAvatarPopup = false;  // deferred: OpenPopup must run
                                               // outside the per-person PushID
                for (const auto& p : roster) {
                    ImGui::PushID(p.name.c_str());
                    // Display picture thumbnail (or placeholder).
                    unsigned tex = avatarTex.get(p.name, p.avatar);
                    ImVec2 cur = ImGui::GetCursorScreenPos();
                    if (tex)
                        ImGui::Image((ImTextureID)(intptr_t)tex,
                                     ImVec2(34, 34));
                    else {
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(cur.x + 17, cur.y + 17), 17,
                            IM_COL32(70, 70, 82, 255));
                        ImGui::Dummy(ImVec2(34, 34));
                    }
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::TextColored(p.hasFace
                                           ? ImVec4(0.5f, 1.0f, 0.6f, 1.0f)
                                           : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                       "%s%s", p.name.c_str(),
                                       p.hasFace ? "  * face" : "");
                    if (ImGui::SmallButton("Picture")) {
                        avatarPicTarget = p.name;
                        avatarTT[0] = avatarURL[0] = '\0';
                        openAvatarPopup = true;  // opened after PopID below
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset landmark"))
                        faceTracker.resetPerson(p.name);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete"))
                        faceTracker.removePerson(p.name);
                    ImGui::EndGroup();
                    ImGui::PopID();
                    ImGui::Separator();
                }
                if (roster.empty())
                    ImGui::TextDisabled("no people yet — add one above");

                // Open here (same ID scope as BeginPopup, i.e. no PushID).
                if (openAvatarPopup) ImGui::OpenPopup("AvatarSrc");

                // Shared "set picture" popup (3 sources; all save locally).
                if (ImGui::BeginPopup("AvatarSrc")) {
                    ImGui::Text("Picture for %s", avatarPicTarget.c_str());
                    ImGui::Separator();
                    ImGui::TextDisabled("Drag an image onto the window,");
                    ImGui::TextDisabled("or use one of these:");
                    ImGui::SetNextItemWidth(180);
                    bool ttGo = ImGui::InputTextWithHint(
                        "##tt", "@tiktok username", avatarTT, sizeof(avatarTT),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Fetch##tt") || ttGo) && avatarTT[0]) {
                        avatarFetcher.request(curProfile.dir, avatarPicTarget,
                                              AvatarFetcher::Source::TikTok,
                                              avatarTT);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetNextItemWidth(180);
                    bool urlGo = ImGui::InputTextWithHint(
                        "##url", "https://image.url", avatarURL,
                        sizeof(avatarURL),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Fetch##url") || urlGo) && avatarURL[0]) {
                        avatarFetcher.request(curProfile.dir, avatarPicTarget,
                                              AvatarFetcher::Source::Url,
                                              avatarURL);
                        ImGui::CloseCurrentPopup();
                    }
                    // While this popup is open, dropped images go to this
                    // person (drained after the frame).
                    avatarDropTarget = avatarPicTarget;
                    ImGui::EndPopup();
                }

                if (!avatarFetcher.status().empty())
                    ImGui::TextDisabled("%s", avatarFetcher.status().c_str());
#else
                ImGui::TextDisabled("face recognition not built in");
#endif
                ImGui::EndChild();
            }
            ImGui::End();
        }
#ifdef DEARTT_FACE_RECOGNITION
        // Route dropped image files to the person whose picture popup is open.
        if (!g_droppedFiles.empty()) {
            if (!avatarDropTarget.empty() && profileLoaded) {
                for (const auto& f : g_droppedFiles)
                    avatarFetcher.request(curProfile.dir, avatarDropTarget,
                                          AvatarFetcher::Source::File, f);
            }
            g_droppedFiles.clear();
        }
#endif

#ifdef DEARTT_STT
        // First-run STT model download: floating progress overlay
        // (bottom-center, on top of everything).
        if (sttDownload.active()) {
            const ImGuiViewport* v = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(v->WorkPos.x + v->WorkSize.x * 0.5f,
                       v->WorkPos.y + v->WorkSize.y - 16.0f),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::Begin("##sttdownload", nullptr,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav);
            ImGui::TextUnformatted("Downloading STT models");
            uint64_t got = sttDownload.downloaded();
            uint64_t tot = sttDownload.total();
            char overlay[64];
            if (tot > 0) {
                std::snprintf(overlay, sizeof(overlay), "%.2f / %.2f GB",
                              got / 1e9, tot / 1e9);
                ImGui::ProgressBar((float)((double)got / (double)tot),
                                   ImVec2(340, 0), overlay);
            } else {
                std::snprintf(overlay, sizeof(overlay), "%.2f GB", got / 1e9);
                // Total unknown yet: indeterminate animation.
                ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                                   ImVec2(340, 0), overlay);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) sttDownload.cancel();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Stop the download. The partial file is kept and the\n"
                    "download resumes on the next launch.");
            ImGui::End();
        }
#endif

        // --- render ---------------------------------------------------------
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

#ifdef DEARTT_FACE_RECOGNITION
    faceTracker.stop();
    avatarTex.shutdown();
#endif
    session.stop();
    player.close();   // joins the decode thread -> no more audio-tap calls
#ifdef DEARTT_STT
    stt.stop();
#endif
    videoTex.destroy();
    giftIcons.shutdown();
    avatars.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
