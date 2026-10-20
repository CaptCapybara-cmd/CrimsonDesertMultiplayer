#include "mod/net/mission_watch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <iterator>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <safetyhook.hpp>

#include "mod/game/safe_memory.h"
#include "mod/log.h"

namespace cdmp::net {
namespace {

constexpr size_t kWindow = 256;  // bytes kept on each side of a key hit
constexpr size_t kMaxHits = 4096;
constexpr DWORD kCompareAfterMs = 300;
constexpr DWORD kWatchMs = 3000;
constexpr int kMaxWriteLogs = 16;

MissionWatchSettings g_cfg;
std::atomic<bool> g_busy{false};

struct Hit {
    uintptr_t addr;
    std::vector<uint8_t> before;  // [addr - kWindow, addr + kWindow)
};
std::vector<Hit> g_hits;

bool copy_seh(uintptr_t src, void* dst, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t scan_region_seh(uintptr_t base, size_t size, uint32_t key, std::vector<uintptr_t>& out) {
    size_t found = 0;
    __try {
        const auto* p = reinterpret_cast<const uint32_t*>(base);
        const size_t n = size / 4;
        for (size_t i = 0; i < n && out.size() < kMaxHits; ++i) {
            if (p[i] == key) {
                out.push_back(base + i * 4);
                ++found;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

std::vector<uintptr_t> scan(uint32_t key, size_t& scanned) {
    std::vector<uintptr_t> out;
    scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    const uintptr_t self = reinterpret_cast<uintptr_t>(&g_cfg);
    while (a < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) == sizeof mbi) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        const bool rw = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && !(mbi.Protect & PAGE_GUARD)) {
            scanned += mbi.RegionSize;
            scan_region_seh(base, mbi.RegionSize, key, out);
        }
        a = base + mbi.RegionSize;
        if (a <= base) break;
    }
    // Our own copies of the key (settings, this vector) are not the game's.
    std::erase_if(out, [&](uintptr_t h) { return h >= self && h < self + sizeof g_cfg; });
    return out;
}

int find_dword(const std::vector<uint8_t>& w, uint32_t v) {
    for (size_t i = 0; i + 4 <= w.size(); i += 4) {
        uint32_t x;
        std::memcpy(&x, w.data() + i, 4);
        if (x == v) return static_cast<int>(i) - static_cast<int>(kWindow);
    }
    return INT32_MIN;
}

DWORD WINAPI compare_later(LPVOID) {
    Sleep(kCompareAfterMs);
    int shown = 0;
    for (const auto& h : g_hits) {
        std::vector<uint8_t> after(2 * kWindow);
        if (!copy_seh(h.addr - kWindow, after.data(), after.size())) continue;
        std::string tmpl, other;
        int changes = 0;
        for (size_t i = 0; i + 4 <= after.size(); i += 4) {
            uint32_t o, n;
            std::memcpy(&o, h.before.data() + i, 4);
            std::memcpy(&n, after.data() + i, 4);
            if (o == n) continue;
            ++changes;
            const int off = static_cast<int>(i) - static_cast<int>(kWindow);
            if (o == 0 && n >= 0x0D0000 && n < 0x0E0000) tmpl += std::format(" {:+d}:0x{:X}", off, n);
            else if (changes <= 12) other += std::format(" {:+d}:{:08X}>{:08X}", off, o, n);
        }
        const int sig = find_dword(h.before, g_cfg.sig);
        if (!tmpl.empty()) {
            log::info("MissionWatch: key at 0x{:X}: TEMPLATE written at{} | signature at {} | {} dwords changed:{}",
                      h.addr, tmpl, sig == INT32_MIN ? std::string("none") : std::format("{:+d}", sig), changes,
                      other);
        } else if (changes && sig != INT32_MIN && shown++ < 12) {
            log::info("MissionWatch: key at 0x{:X} (signature at {:+d}): {} dwords changed:{}", h.addr, sig, changes,
                      other);
        }
    }
    log::info("MissionWatch: compare done ({} key hits)", g_hits.size());
    g_hits.clear();
    g_busy = false;
    return 0;
}

// --- mode 2: hardware write watch on the server thread ---
std::atomic<int> g_write_logs{0};
bool g_watch_reads = false;  // R/W = 11 (read or write) instead of 01 (write)
uintptr_t g_watch[4] = {};
int g_nwatch = 0;
PVOID g_veh = nullptr;
uintptr_t g_exe_lo = 0, g_exe_hi = 0;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    // Wine does not always fill Dr6: while our watch is armed, a single step is ours.
    if (!(dr6 & 0xF)) {
        if (!g_nwatch || !(c->Dr7 & 0xFF)) return EXCEPTION_CONTINUE_SEARCH;
        dr6 = (1ull << g_nwatch) - 1;
    }
    if (g_write_logs.fetch_add(1) < kMaxWriteLogs) {
        std::string stack;
        int n = 0;
        for (int i = 0; i < 96 && n < 12; ++i) {
            uintptr_t v = 0;
            if (!copy_seh(c->Rsp + i * 8, &v, 8)) break;
            if (v >= g_exe_lo && v < g_exe_hi) {
                stack += std::format(" +0x{:X}", v - g_exe_lo + 0x1000);
                ++n;
            }
        }
        for (int i = 0; i < g_nwatch; ++i) {
            if (!(dr6 & (1ull << i))) continue;
            uint32_t now = 0;
            copy_seh(g_watch[i], &now, 4);
            log::info("MissionWatch: {} 0x{:X} (now 0x{:X}) by exe+0x{:X} on thread {} | stack:{}",
                      g_watch_reads ? "ACCESS to" : "WRITE to", g_watch[i], now, c->Rip - g_exe_lo + 0x1000,
                      GetCurrentThreadId(), stack);
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool set_debug_regs(HANDLE thread, bool on) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return false;
    bool ok = GetThreadContext(thread, &c);
    if (ok) {
        DWORD64* drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
        c.Dr7 = 0;
        for (int i = 0; i < 4; ++i) *drs[i] = 0;
        if (on) {
            for (int i = 0; i < g_nwatch; ++i) {
                *drs[i] = g_watch[i];
                // L_i enable, R/W = 01 (write), LEN = 11 (4 bytes)
                c.Dr7 |= (1ull << (i * 2)) | ((g_watch_reads ? 3ull : 1ull) << (16 + i * 4)) | (3ull << (18 + i * 4));
            }
        }
        c.Dr6 = 0;
        ok = SetThreadContext(thread, &c);
    }
    ResumeThread(thread);
    return ok;
}

struct ArmArgs {
    DWORD server_tid;
    HANDLE done;
};

DWORD WINAPI arm_thread(LPVOID p) {
    auto* a = static_cast<ArmArgs*>(p);
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, a->server_tid);
    const bool ok = t && set_debug_regs(t, true);
    log::info("MissionWatch: hardware write watch on {} address(es), thread {}: {}", g_nwatch, a->server_tid,
              ok ? "armed" : "FAILED");
    SetEvent(a->done);
    if (!t) return 0;
    Sleep(kWatchMs);
    const bool off = set_debug_regs(t, false);
    log::info("MissionWatch: watch removed ({}), {} write(s) seen", off ? "ok" : "FAILED", g_write_logs.load());
    CloseHandle(t);
    g_busy = false;
    return 0;
}

// --- mode 3: force the template in the server's mission object copy ---
constexpr uintptr_t kMissionCopy = 0x1A9B4A0;
constexpr uint8_t kMissionCopyHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                        0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
constexpr size_t kKeyOff = 0x28, kObjectBytes = 0xD8;
safetyhook::MidHook g_copy_hook;
std::atomic<int> g_forced{0};

bool forced_key(uint32_t key) {
    if (key == g_cfg.key) return true;
    for (uint32_t k : g_cfg.force_more) {
        if (k && k == key) return true;
    }
    return false;
}

void on_mission_copy(safetyhook::Context& ctx) {
    const uintptr_t src = ctx.rdx;
    uint32_t key = 0;
    if (!copy_seh(src + kKeyOff, &key, 4) || !forced_key(key)) return;
    uint32_t obj[kObjectBytes / 4];
    if (!copy_seh(src, obj, sizeof obj)) return;
    std::string changed;
    for (size_t i = 0; i < kObjectBytes / 4; ++i) {
        const uint32_t v = obj[i];
        if (i * 4 == kKeyOff || v < 0x0D0000 || v >= 0x0E0000 || v == g_cfg.force_template) continue;
        __try {
            reinterpret_cast<uint32_t*>(src)[i] = g_cfg.force_template;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        changed += std::format(" +0x{:X}:0x{:X}", i * 4, v);
    }
    if (!changed.empty() && g_forced.fetch_add(1) < 16) {
        log::info("MissionWatch: FORCED slot 0x{:X} to template 0x{:X} at{} (src 0x{:X}, dst 0x{:X}, thread {})", key,
                  g_cfg.force_template, changed, src, ctx.rcx, GetCurrentThreadId());
    }
}

}  // namespace

void mission_watch_configure(const MissionWatchSettings& s) {
    g_cfg = s;
    if (s.mode) {
        log::warn("MissionWatch: mode {} (key 0x{:X}, signature 0x{:X} at {:+d}, template at {:+d}); test copy only",
                  s.mode, s.key, s.sig, s.sig_off, s.tmpl_off);
    }
    if (s.mode == 3 && !g_copy_hook) {
        const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uint8_t head[sizeof kMissionCopyHead];
        if (!game::safe_read(exe + kMissionCopy, head, sizeof head) || std::memcmp(head, kMissionCopyHead, sizeof head)) {
            log::error("MissionWatch: mission copy prologue doesn't match; force not installed");
            return;
        }
        g_copy_hook = safetyhook::create_mid(reinterpret_cast<void*>(exe + kMissionCopy), on_mission_copy);
        log::info("MissionWatch: force {}: slot 0x{:X} gets template 0x{:X}", g_copy_hook ? "installed" : "FAILED",
                  s.key, s.force_template);
    }
}

// --- mode 4: the variant pool ---
// Templates seen for Renee's Request in 13 sessions (NOTES.md 2026-10-20).
constexpr uint32_t kSeenVariants[] = {0x0D2147, 0x0D214C, 0x0D2150, 0x0D2153, 0x0D2158, 0x0D215D, 0x0D2197,
                                      0x0D219C, 0x0D219D, 0x0D21A5, 0x0D21C5, 0x0D2DA7};
constexpr size_t kClusterBytes = 0x100000;
const void* g_self_module = [] {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&kClusterBytes), &m);
    return static_cast<const void*>(m);
}();

bool is_seen_variant(uint32_t v) {
    for (uint32_t k : kSeenVariants) {
        if (k == v) return true;
    }
    return false;
}

void scan_region_variants(uintptr_t base, size_t size, std::vector<std::pair<uintptr_t, uint32_t>>& out) {
    __try {
        const auto* p = reinterpret_cast<const uint32_t*>(base);
        for (size_t i = 0; i < size / 4 && out.size() < 200000; ++i) {
            const uint32_t v = p[i];
            if (v >= 0x0D2147 && v <= 0x0D2DA7 && is_seen_variant(v)) out.push_back({base + i * 4, v});
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::vector<std::pair<uintptr_t, uint32_t>> find_pool() {
    const uint64_t t0 = GetTickCount64();
    std::vector<std::pair<uintptr_t, uint32_t>> hits;
    MEMORY_BASIC_INFORMATION mbi;
    size_t scanned = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull &&
                                VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) == sizeof mbi;) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                              prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE;
        // Not our own module (its kSeenVariants table matches itself).
        const bool ours = mbi.AllocationBase == g_self_module;
        if (mbi.State == MEM_COMMIT && readable && !(mbi.Protect & PAGE_GUARD) && !ours) {
            scanned += mbi.RegionSize;
            scan_region_variants(base, mbi.RegionSize, hits);
        }
        a = base + mbi.RegionSize;
        if (a <= base) break;
    }
    log::info("MissionWatch: pool: {} dwords equal to a seen variant in {} MB ({} ms)", hits.size(), scanned >> 20,
              GetTickCount64() - t0);
    // Every hit (few expected), with its memory type: the layout tells what holds them.
    for (size_t i = 0; i < hits.size() && i < 120; ++i) {
        MEMORY_BASIC_INFORMATION m{};
        VirtualQuery(reinterpret_cast<void*>(hits[i].first), &m, sizeof m);
        uint8_t ctx[32] = {};
        copy_seh(hits[i].first - 8, ctx, sizeof ctx);
        std::string h;
        for (uint8_t b : ctx) h += std::format("{:02X}", b);
        log::info("MissionWatch: pool hit 0x{:X} = 0x{:X} (type 0x{:X}, prot 0x{:X}, alloc 0x{:X}) | -8: {}", hits[i].first,
                  hits[i].second, m.Type, m.Protect, reinterpret_cast<uintptr_t>(m.AllocationBase), h);
    }
    // Clusters: hits within kClusterBytes of each other; rank by distinct variants.
    struct Cluster {
        uintptr_t lo, hi;
        uint32_t distinct;
        size_t count;
    };
    std::vector<Cluster> cl;
    for (size_t i = 0; i < hits.size();) {
        size_t j = i;
        while (j + 1 < hits.size() && hits[j + 1].first - hits[j].first < kClusterBytes) ++j;
        uint32_t mask = 0;
        for (size_t k = i; k <= j; ++k) {
            for (size_t v = 0; v < std::size(kSeenVariants); ++v) {
                if (hits[k].second == kSeenVariants[v]) mask |= 1u << v;
            }
        }
        const uint32_t distinct = static_cast<uint32_t>(__popcnt(mask));
        if (distinct >= 3) cl.push_back({hits[i].first, hits[j].first, distinct, j - i + 1});
        i = j + 1;
    }
    std::sort(cl.begin(), cl.end(), [](const Cluster& a, const Cluster& b) { return a.distinct > b.distinct; });
    for (size_t c = 0; c < cl.size() && c < 12; ++c) {
        std::string offs;
        size_t shown = 0;
        uintptr_t prev = 0;
        for (const auto& [addr, v] : hits) {
            if (addr < cl[c].lo || addr > cl[c].hi || shown >= 24) continue;
            offs += std::format(" +0x{:X}:{:X}{}", addr - cl[c].lo, v,
                                prev ? std::format("(d{:X})", addr - prev) : std::string());
            prev = addr;
            ++shown;
        }
        MEMORY_BASIC_INFORMATION m{};
        VirtualQuery(reinterpret_cast<void*>(cl[c].lo), &m, sizeof m);
        log::info("MissionWatch: pool cluster 0x{:X}-0x{:X}: {} distinct variants, {} hits, type 0x{:X} prot 0x{:X}:{}",
                  cl[c].lo, cl[c].hi, cl[c].distinct, cl[c].count, m.Type, m.Protect, offs);
    }
    if (!cl.empty()) {
        uint8_t dump[0x100];
        if (copy_seh(cl[0].lo - 0x40, dump, sizeof dump)) {
            std::string h;
            for (size_t i = 0; i < sizeof dump; ++i) h += std::format("{:02X}{}", dump[i], (i % 32 == 31) ? " | " : "");
            log::info("MissionWatch: pool best -0x40: {}", h);
        }
    }
    return hits;
}

void mission_watch_on_accept() {
    if (g_cfg.mode == 4) {
        if (!g_busy.exchange(true)) {
            find_pool();
            g_busy = false;
        }
        return;
    }
    if (g_cfg.mode == 5) {
        if (g_busy.exchange(true)) return;
        const auto hits = find_pool();
        // The private allocation with the most distinct variants holds the data records.
        std::map<uintptr_t, uint32_t> masks;
        for (const auto& [a, v] : hits) {
            MEMORY_BASIC_INFORMATION m{};
            VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m);
            if (m.Type != MEM_PRIVATE) continue;
            for (size_t i = 0; i < std::size(kSeenVariants); ++i) {
                if (kSeenVariants[i] == v) masks[reinterpret_cast<uintptr_t>(m.AllocationBase)] |= 1u << i;
            }
        }
        uintptr_t best = 0;
        for (const auto& [b, m] : masks) {
            if (!best || __popcnt(m) > __popcnt(masks[best])) best = b;
        }
        g_nwatch = 0;
        uint32_t used = 0;
        for (const auto& [a, v] : hits) {
            MEMORY_BASIC_INFORMATION m{};
            VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m);
            if (reinterpret_cast<uintptr_t>(m.AllocationBase) != best || g_nwatch >= 4) continue;
            for (size_t i = 0; i < std::size(kSeenVariants); ++i) {
                if (kSeenVariants[i] == v && !(used & (1u << i))) {
                    used |= 1u << i;
                    g_watch[g_nwatch++] = a;
                    log::info("MissionWatch: read-watching 0x{:X} (variant 0x{:X}'s record)", a, v);
                }
            }
        }
        if (!g_nwatch) {
            g_busy = false;
            return;
        }
        g_watch_reads = true;
        const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        g_exe_lo = exe + 0x1000;
        g_exe_hi = exe + 0x170D4000;
        g_write_logs = 0;
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_exception);
        ArmArgs args{GetCurrentThreadId(), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (HANDLE t = CreateThread(nullptr, 0, arm_thread, &args, 0, nullptr)) {
            CloseHandle(t);
            WaitForSingleObject(args.done, 5000);
        } else {
            g_busy = false;
        }
        CloseHandle(args.done);
        return;
    }
    if (!g_cfg.mode || g_cfg.mode == 3 || g_busy.exchange(true)) return;
    const uint64_t t0 = GetTickCount64();
    size_t scanned = 0;
    const auto hits = scan(g_cfg.key, scanned);
    log::info("MissionWatch: {} dwords == 0x{:X} in {} MB of private RW memory ({} ms)", hits.size(), g_cfg.key,
              scanned >> 20, GetTickCount64() - t0);
    if (g_cfg.mode == 1) {
        g_hits.clear();
        for (uintptr_t h : hits) {
            Hit x{h, std::vector<uint8_t>(2 * kWindow)};
            if (copy_seh(h - kWindow, x.before.data(), x.before.size())) g_hits.push_back(std::move(x));
        }
        if (HANDLE t = CreateThread(nullptr, 0, compare_later, nullptr, 0, nullptr)) CloseHandle(t);
        return;
    }
    // mode 2: watch the template field of the hits whose signature matches.
    g_nwatch = 0;
    for (uintptr_t h : hits) {
        uint32_t sig = 0;
        if (g_nwatch < 4 && copy_seh(h + g_cfg.sig_off, &sig, 4) && sig == g_cfg.sig) {
            g_watch[g_nwatch++] = h + g_cfg.tmpl_off;
            log::info("MissionWatch: watching 0x{:X} (key at 0x{:X})", h + g_cfg.tmpl_off, h);
        }
    }
    if (!g_nwatch) {
        log::warn("MissionWatch: no key hit has the signature; nothing watched");
        g_busy = false;
        return;
    }
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_exe_lo = exe + 0x1000;
    g_exe_hi = exe + 0x170D4000;
    g_write_logs = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_exception);
    // Debug registers of a running thread are set from another thread while it waits here.
    ArmArgs args{GetCurrentThreadId(), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (HANDLE t = CreateThread(nullptr, 0, arm_thread, &args, 0, nullptr)) {
        CloseHandle(t);
        WaitForSingleObject(args.done, 5000);
    } else {
        g_busy = false;
    }
    CloseHandle(args.done);
}

}  // namespace cdmp::net
