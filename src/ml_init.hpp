#pragma once

// Serializes first-touch initialization of the ML runtimes (ONNX Runtime's
// device discovery and ggml's Vulkan backend bootstrap).
//
// Both the face tracker and the STT engine initialize their runtime on their
// own worker thread at startup, and both runtimes create a Vulkan instance
// during that init. Doing this concurrently trips a lazy-init race in Wine's
// winevulkan (assert at loader.c:319 kills the thread -> "loading model"
// forever), and concurrent first-init of GPU driver stacks is a known source
// of flakiness on real Windows drivers too. Holding this mutex during init
// costs nothing measurable and removes the race everywhere.

#include <mutex>

inline std::mutex& mlInitMutex() {
    static std::mutex m;
    return m;
}
