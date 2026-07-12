cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# DearTT — a cross-platform (Windows / Linux / macOS) TikTok LIVE viewer.
#
#   +-----------------------------+----------------+
#   |                             |  chat / gifts  |
#   |   live video (FFmpeg)       |  (ttlive-cpp)  |
#   |                             |                |
#   +-----------------------------+----------------+
#
# Vendored (cloned) libraries — built from source on every platform:
#   imgui/               Dear ImGui (docking-free stock clone)
#   third_party/glfw/    GLFW 3.4 (window + GL context + input)
#   ttlive-cpp/          TikTok LIVE event client (chat, gifts, stream URLs)
#
# Prebuilt / system libraries:
#   FFmpeg (avformat/avcodec/swscale/avutil)  — HLS/FLV demux + H.264 decode.
#     Linux/macOS: found via pkg-config (apt: libavformat-dev libavcodec-dev
#                  libswscale-dev libavutil-dev / brew: ffmpeg)
#     Windows:     pass -DFFMPEG_DIR=<prefix of a shared ffmpeg build>
#                  (e.g. the BtbN ffmpeg-master-latest-win64-gpl-shared zip)
#   OpenSSL, protobuf, zlib — required by ttlive-cpp (vcpkg on Windows).
# ---------------------------------------------------------------------------

project(DearTT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# ttlive-cpp — TikTok LIVE events + stream URLs
# ---------------------------------------------------------------------------
add_subdirectory(ttlive-cpp)

# ---------------------------------------------------------------------------
# GLFW (vendored) — window / GL context
# ---------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw)

# ---------------------------------------------------------------------------
# FreeType — required for color emoji (COLR/CBDT), which stb_truetype cannot
# rasterize. System package on Linux/macOS (built with libpng, needed for
# Noto Color Emoji's PNG bitmaps); vendored static build for the Windows
# cross-compile (Segoe UI Emoji is COLR vector layers — no libpng needed).
# ---------------------------------------------------------------------------
if(WIN32)
  set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
  add_subdirectory(third_party/freetype)
  set(DEARTT_FREETYPE freetype)
else()
  find_package(Freetype REQUIRED)
  set(DEARTT_FREETYPE Freetype::Freetype)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui (vendored) — core + GLFW/OpenGL3 backends + FreeType rasterizer
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# WCHAR32: without it ImWchar is 16-bit and every supplementary-plane
# codepoint — i.e. nearly all emoji — decodes to U+FFFD before font lookup.
# PUBLIC: it changes ImWchar's size (ABI), all TUs must agree.
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_USE_WCHAR32)
target_link_libraries(imgui PUBLIC glfw ${DEARTT_FREETYPE})

# ImPlot (vendored) — plots for the stats panel.
add_library(implot STATIC
  third_party/implot/implot.cpp
  third_party/implot/implot_items.cpp
)
target_include_directories(implot PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)
target_link_libraries(implot PUBLIC imgui)

# ---------------------------------------------------------------------------
# FFmpeg — avformat / avcodec / swscale / avutil
# ---------------------------------------------------------------------------
set(FFMPEG_DIR "" CACHE PATH "Prefix of a prebuilt FFmpeg (include/ + lib/)")

if(FFMPEG_DIR)
  # Explicit prebuilt FFmpeg (typical on Windows).
  find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
            HINTS ${FFMPEG_DIR}/include REQUIRED)
  foreach(_lib avformat avcodec swscale swresample avutil)
    find_library(FFMPEG_${_lib}_LIB ${_lib}
                 HINTS ${FFMPEG_DIR}/lib REQUIRED)
  endforeach()
  add_library(ffmpeg INTERFACE)
  target_include_directories(ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
  target_link_libraries(ffmpeg INTERFACE
    ${FFMPEG_avformat_LIB} ${FFMPEG_avcodec_LIB}
    ${FFMPEG_swscale_LIB} ${FFMPEG_swresample_LIB} ${FFMPEG_avutil_LIB})
else()
  # System FFmpeg via pkg-config (Linux / macOS).
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libswscale libswresample libavutil)
  add_library(ffmpeg INTERFACE)
  target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# miniaudio (vendored) — audio playback device (WASAPI/CoreAudio/ALSA/Pulse).
# Header-only; backends are loaded at runtime so no extra system libs are
# needed beyond dl/pthread on POSIX.
# ---------------------------------------------------------------------------
add_library(miniaudio STATIC src/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
if(UNIX AND NOT APPLE)
  target_link_libraries(miniaudio PUBLIC dl pthread m)
elseif(APPLE)
  target_link_libraries(miniaudio PUBLIC pthread m)
endif()

# ---------------------------------------------------------------------------
# civetweb (vendored) — embedded HTTP + WebSocket server for the event
# forwarder (ws://:8080/ws) and the bundled website (web/). Compiled directly
# (its own CMake drags in tests/SSL); websockets on, TLS/CGI off.
# ---------------------------------------------------------------------------
add_library(civetweb STATIC third_party/civetweb/src/civetweb.c)
target_include_directories(civetweb PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/civetweb/include)
target_compile_definitions(civetweb PUBLIC USE_WEBSOCKET NO_SSL NO_CGI)
if(WIN32)
  target_link_libraries(civetweb PUBLIC ws2_32)
elseif(UNIX)
  target_link_libraries(civetweb PUBLIC pthread)
endif()

# nlohmann/json (vendored single header) — JSON envelopes for the forwarder.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)

# Protobuf for event_json.cpp (reflection + json_util on Webcast payloads).
# ttlive-cpp finds it too, but its IMPORTED target is directory-scoped.
find_package(Protobuf CONFIG QUIET)
if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf REQUIRED)
endif()

# ---------------------------------------------------------------------------
# OpenGL
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# DearTT application
# ---------------------------------------------------------------------------
add_executable(deartt
  src/main.cpp
  src/video_player.cpp
  src/audio_output.cpp
  src/live_session.cpp
  src/event_server.cpp
  src/event_json.cpp
  src/stats.cpp
  src/icon_cache.cpp
)
target_include_directories(deartt PRIVATE src
  # ttlive internals: http_client.hpp for gift-icon downloads (vendored lib,
  # implementation already linked via ttlive).
  ${CMAKE_CURRENT_SOURCE_DIR}/ttlive-cpp/src)
target_link_libraries(deartt PRIVATE
  imgui
  implot
  glfw
  ffmpeg
  ttlive
  miniaudio
  civetweb
  nlohmann_json
  # Full protobuf runtime (already required by ttlive): event_json.cpp uses
  # reflection + json_util to decode raw Webcast payloads generically.
  protobuf::libprotobuf
  OpenGL::GL
)
# Development fallback for the served website (deployed installs use web/
# next to the exe — see findWebDir / scripts/package-win64.sh).
target_compile_definitions(deartt PRIVATE
  DEARTT_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
  DEARTT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
  target_compile_definitions(deartt PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # GUI subsystem: no console window (MinGW links main() fine with -mwindows).
  set_target_properties(deartt PROPERTIES WIN32_EXECUTABLE ON)
endif()
if(APPLE)
  # ImGui's OpenGL3 backend on macOS uses the deprecated (but working) GL.
  target_compile_definitions(deartt PRIVATE GL_SILENCE_DEPRECATION)
endif()
