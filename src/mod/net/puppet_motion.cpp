#include "mod/net/puppet_motion.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

#include "mod/game/actor_lookup.h"
#include "mod/log.h"

namespace cdmp::net {
namespace {

constexpr uint32_t kPuppetPlayer = 0xA010F001;  // the copy's key (remote_puppet.cpp)
constexpr uint64_t kSampleMs = 20;
constexpr float kJumpSpeed = 15.0f;        // m/s
// ...and a real distance: at a run, two or three game frames between samples
// already cover 0.3-0.6 m, which is not a teleport.
constexpr float kJumpMinStep = 0.75f;      // m
constexpr float kCatchUpSpeed = 10.0f;     // m/s, right after a stall
constexpr uint64_t kStallMs = 150;
constexpr float kStillM = 0.01f;
// The copy's first position updates after a rest snap it to where the sender is: not counted.
constexpr uint64_t kWarmUpMs = 3000;

struct Stats {
    bool on = false;
    uint64_t opened_ms = 0;
    uint64_t samples = 0, jumps = 0, stall_jumps = 0;
    float travelled = 0.0f, max_speed = 0.0f, max_step = 0.0f;
};

std::mutex g_mutex;

// GetTickCount64 ticks every ~16 ms: too coarse to time 20 ms samples.
uint64_t now_us() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart * 1000000 / freq);
}
Stats g_stats;
std::optional<proto::Vec3> g_last;
uint64_t g_last_ms = 0, g_last_us = 0, g_still_since = 0;

}  // namespace

void puppet_motion_window(bool on) {
    std::lock_guard lock(g_mutex);
    if (on) {
        g_stats = Stats{};
        g_stats.opened_ms = now_us() / 1000;
    }
    g_stats.on = on;
    g_last.reset();
    g_still_since = 0;
}

void puppet_motion_sample() {
    const uint64_t now = now_us() / 1000;
    const uint64_t now_fine = now_us();
    {
        std::lock_guard lock(g_mutex);
        if (!g_stats.on || now - g_last_ms < kSampleMs) return;
    }
    const auto pos = game::actor_world_position(kPuppetPlayer);
    std::lock_guard lock(g_mutex);
    if (!g_stats.on) return;
    if (!pos) {
        g_last.reset();
        return;
    }
    if (now - g_stats.opened_ms < kWarmUpMs) {
        g_last = pos;
        g_last_ms = now;
        g_last_us = now_fine;
        g_still_since = 0;
        return;
    }
    if (g_last) {
        const float dt = static_cast<float>(now_fine - g_last_us) / 1000000.0f;
        const float step = std::hypot(pos->x - g_last->x, pos->z - g_last->z);
        const float speed = dt > 0 ? step / dt : 0.0f;
        ++g_stats.samples;
        g_stats.travelled += step;
        g_stats.max_step = std::max(g_stats.max_step, step);
        if (dt <= 0.06f) g_stats.max_speed = std::max(g_stats.max_speed, speed);
        if (step < kStillM) {
            if (!g_still_since) g_still_since = g_last_ms;
        } else {
            if (dt <= 0.06f && speed > kJumpSpeed && step > kJumpMinStep) {
                ++g_stats.jumps;
                if (g_stats.jumps <= 10) log::info("Puppet motion: jump of {:.2f} m in {:.0f} ms", step, dt * 1000.0f);
            } else if (g_still_since && g_last_ms - g_still_since >= kStallMs && dt <= 0.06f && speed > kCatchUpSpeed) {
                ++g_stats.stall_jumps;
                if (g_stats.stall_jumps <= 10) {
                    log::info("Puppet motion: stood still {} ms, then {:.2f} m in {:.0f} ms", g_last_ms - g_still_since,
                              step, dt * 1000.0f);
                }
            }
            g_still_since = 0;
        }
    }
    g_last = pos;
    g_last_ms = now;
    g_last_us = now_fine;
}

void puppet_motion_report(float walked_m) {
    Stats s;
    {
        std::lock_guard lock(g_mutex);
        s = g_stats;
    }
    const char* result = walked_m < 5.0f                      ? "SKIP"
                         : s.samples < 50                     ? "SKIP"
                         : s.jumps || s.stall_jumps           ? "FAIL"
                         : s.travelled < 0.5f * walked_m      ? "FAIL"
                                                              : "PASS";
    log::info("Puppet motion: RESULT: {} | our walk {:.1f} m, copy moved {:.1f} m in {} samples, max {:.1f} m/s | "
              "largest step {:.2f} m | jumps {} | stood still then jumped {}",
              result, walked_m, s.travelled, s.samples, s.max_speed, s.max_step, s.jumps, s.stall_jumps);
}

}  // namespace cdmp::net
