#pragma once

#include <atomic>
#include <cstdint>

namespace owc {

// Cross-thread state. Each field gets its own cache line to avoid false sharing
// (the grabber writes aimData continuously while the aim thread reads it).
struct Shared {
    alignas(64) std::atomic<bool> running{true};
    alignas(64) std::atomic<uint64_t> aimData{0};
    alignas(64) std::atomic<bool> toggleUI{false};
};

} // namespace owc