#include "mod/net/quest_rng.h"

#include <windows.h>
#include <intrin.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/mission_watch.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;

constexpr uintptr_t kQuestDialogEndHandler = 0x2A4C510;  // QuestDialogEndReq Process (vf2)
constexpr uintptr_t kRandState = 0x1378F10;              // uint32_t* (): this thread's rand state, seeded on first use
constexpr uint8_t kHandlerHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57,
                                    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr uint8_t kRandStateHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                      0xEC, 0x20, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58};

// Diagnostics: for kTraceMs after a pinned answer, who draws from the rand and where the candidates start.
constexpr uintptr_t kChanceGate = 0x22CF730;   // rand32 % 1e6 < rate -> nothing; else 0x22CFDD0 per sub-entry
constexpr uintptr_t kWeightedPick = 0x22CFDD0;  // weighted pick over 0x70-byte entries
constexpr uint8_t kPickHead[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44};
constexpr uint64_t kTraceMs = 3000;
constexpr size_t kTraceFirst = 80;

using RandStateFn = uint32_t*(__fastcall*)();
RandStateFn g_rand_state = nullptr;
safetyhook::MidHook g_hook;
safetyhook::MidHook g_draw_hook, g_gate_hook, g_pick_hook;
std::atomic<bool> g_tracing{false};
uint64_t g_trace_until = 0;
uintptr_t g_base = 0;
SRWLOCK g_trace_lock = SRWLOCK_INIT;
std::map<std::pair<DWORD, uintptr_t>, uint32_t> g_draws;  // (thread, caller rva) -> count
std::vector<std::pair<DWORD, uintptr_t>> g_first;
uint32_t g_draws_total = 0;
// The handler thread's own draws, in order, runs of one caller merged: (caller rva, count, ms since the answer).
DWORD g_trace_thread = 0;
uint64_t g_trace_start = 0;
struct Run {
    uintptr_t caller;
    uint32_t count;
    uint32_t ms;
    uint32_t state;  // this thread's rand state when the run began (before its first draw)
};

// Same place the getter reads: TLS block 0 + 0x4AD8 (valid once seeded).
uint32_t peek_state() {
    const auto tls = *reinterpret_cast<uintptr_t*>(__readgsqword(0x58));
    return *reinterpret_cast<const uint32_t*>(tls + 0x4AD8);
}
std::vector<Run> g_order;
constexpr size_t kOrderMax = 400;

uintptr_t caller_rva(const safetyhook::Context& ctx) {
    const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(ctx.rsp);
    return ret >= g_base ? ret - g_base : ret;
}

void on_draw(safetyhook::Context& ctx) {
    if (!g_tracing.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() > g_trace_until) {
        g_tracing = false;
        return;
    }
    const std::pair<DWORD, uintptr_t> k{GetCurrentThreadId(), caller_rva(ctx)};
    AcquireSRWLockExclusive(&g_trace_lock);
    ++g_draws[k];
    ++g_draws_total;
    if (g_first.size() < kTraceFirst) g_first.push_back(k);
    if (k.first == g_trace_thread) {
        if (!g_order.empty() && g_order.back().caller == k.second) {
            ++g_order.back().count;
        } else if (g_order.size() < kOrderMax) {
            g_order.push_back({k.second, 1, static_cast<uint32_t>(GetTickCount64() - g_trace_start), peek_state()});
        }
    }
    ReleaseSRWLockExclusive(&g_trace_lock);
}

void on_candidate(const char* what, safetyhook::Context& ctx) {
    if (!g_tracing.load(std::memory_order_relaxed)) return;
    // This thread's state, without drawing (the getter only seeds on first use).
    const uint32_t state = *g_rand_state();
    log::info("QuestRng: trace: {} entered on thread {} from exe+0x{:X}, rand state 0x{:08X}", what,
              GetCurrentThreadId(), caller_rva(ctx), state);
}

DWORD WINAPI dump_trace(LPVOID) {
    Sleep(static_cast<DWORD>(kTraceMs + 500));
    AcquireSRWLockExclusive(&g_trace_lock);
    log::info("QuestRng: trace: {} rand draws in {} ms after the pinned answer", g_draws_total, kTraceMs);
    for (const auto& [k, n] : g_draws) log::info("QuestRng: trace:   thread {} caller exe+0x{:X} x{}", k.first, k.second, n);
    std::string order;
    for (const auto& [t, r] : g_first) order += std::format(" {}:{:X}", t, r);
    log::info("QuestRng: trace: first draws (thread:caller):{}", order);
    std::string seq;
    for (const auto& r : g_order) seq += std::format(" {}ms:{:X}x{}@{:08X}", r.ms, r.caller, r.count, r.state);
    log::info("QuestRng: trace: handler thread {} draws in order:{}", g_trace_thread, seq);
    g_order.clear();
    g_draws.clear();
    g_first.clear();
    g_draws_total = 0;
    ReleaseSRWLockExclusive(&g_trace_lock);
    return 0;
}
std::atomic<bool> g_armed{false};
std::atomic<uint32_t> g_pin{0};
std::atomic<uint32_t> g_last{0};

bool head_matches(uintptr_t addr, const uint8_t* expected, size_t n) {
    uint8_t actual[16];
    return n <= sizeof actual && game::safe_read(addr, actual, n) && std::memcmp(actual, expected, n) == 0;
}

void on_handler(safetyhook::Context&) {
    uint32_t* state = g_rand_state();
    if (g_armed.exchange(false)) {
        const uint32_t was = *state;
        *state = g_pin.load();
        log::info("QuestRng: QuestDialogEndReq on thread {}: rand state 0x{:08X} pinned to 0x{:08X}",
                  GetCurrentThreadId(), was, *state);
        if (g_draw_hook) {
            g_trace_thread = GetCurrentThreadId();
            g_trace_start = GetTickCount64();
            g_trace_until = GetTickCount64() + kTraceMs;
            g_tracing = true;
            if (HANDLE t = CreateThread(nullptr, 0, dump_trace, nullptr, 0, nullptr)) CloseHandle(t);
        }
    } else {
        log::info("QuestRng: QuestDialogEndReq on thread {}: rand state 0x{:08X}", GetCurrentThreadId(), *state);
    }
    g_last = *state;
    mission_watch_on_accept();
}

}  // namespace

bool quest_rng_install(bool trace) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (base != off::kImageBase || nt->FileHeader.TimeDateStamp != off::kPeTimestamp) {
        log::warn("QuestRng: unsupported game build, not installed");
        return false;
    }
    if (!head_matches(base + kQuestDialogEndHandler, kHandlerHead, sizeof kHandlerHead) ||
        !head_matches(base + kRandState, kRandStateHead, sizeof kRandStateHead)) {
        log::error("QuestRng: function prologues don't match the expected bytes, not installed");
        return false;
    }
    g_rand_state = reinterpret_cast<RandStateFn>(base + kRandState);
    g_hook = safetyhook::create_mid(reinterpret_cast<void*>(base + kQuestDialogEndHandler), on_handler);
    if (!g_hook) {
        log::error("QuestRng: hook creation failed");
        return false;
    }
    log::info("QuestRng: hooked QuestDialogEndReq's handler (exe+0x{:X})", kQuestDialogEndHandler);
    g_base = base;
    if (!trace) return true;
    if (head_matches(base + kChanceGate, kPickHead, sizeof kPickHead) &&
        head_matches(base + kWeightedPick, kPickHead, sizeof kPickHead)) {
        g_gate_hook = safetyhook::create_mid(reinterpret_cast<void*>(base + kChanceGate),
                                             [](safetyhook::Context& c) { on_candidate("chance gate 0x22CF730", c); });
        g_pick_hook = safetyhook::create_mid(reinterpret_cast<void*>(base + kWeightedPick),
                                             [](safetyhook::Context& c) { on_candidate("weighted pick 0x22CFDD0", c); });
    }
    g_draw_hook = safetyhook::create_mid(reinterpret_cast<void*>(base + kRandState), on_draw);
    log::info("QuestRng: trace hooks: draws {} gate {} pick {}", static_cast<bool>(g_draw_hook),
              static_cast<bool>(g_gate_hook), static_cast<bool>(g_pick_hook));
    return true;
}

void quest_rng_pin_next(uint32_t state) {
    g_pin = state;
    g_armed = true;
}

uint32_t quest_rng_last_state() { return g_last.load(); }

}  // namespace cdmp::net
