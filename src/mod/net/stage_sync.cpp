#include "mod/net/stage_sync.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <string>

#include "common/stage_sync.h"
#include "mod/log.h"
#include "mod/net/stage_tracker.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

constexpr uint32_t kFeedAfterMs = 20000;      // in the world this long before the test feed goes out
constexpr uint32_t kFeedGiveUpMs = 6 * 60 * 1000;
constexpr uint32_t kSurviveMs = 30000;

StageSyncSettings g_cfg;
StageSyncQueue g_queue;
bool g_was_loading = false;
bool g_collecting = false;
uint32_t g_in_world_since = 0;
uint32_t g_last_stats = 0;
StageSyncQueue::Counters g_logged{};

// Echo test state.
enum class Feed { Waiting, Sent, Applied, Done };
Feed g_feed = Feed::Waiting;
uint32_t g_feed_ms = 0;
uint32_t g_applied_ms = 0;

std::vector<proto::StageBranchMsg> load_feed(const std::wstring& path) {
    // Every recorded branch, in order: a scene runs through several (the queue chains them).
    std::vector<proto::StageBranchMsg> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (const auto b = parse_branch_line(line)) out.push_back(*b);
    }
    return out;
}

void quit_later() {
    CloseHandle(CreateThread(
        nullptr, 0, [](LPVOID) -> DWORD { test_quit_game(20000); return 0; }, nullptr, 0, nullptr));
}

void feed_test(bool in_world, uint32_t now_ms, std::vector<proto::StageBranchMsg>& out) {
    if (g_cfg.feed_path.empty() || g_feed == Feed::Done) return;
    const auto& c = g_queue.counters();
    switch (g_feed) {
        case Feed::Waiting:
            if (in_world && g_in_world_since && now_ms - g_in_world_since >= kFeedAfterMs) {
                auto msgs = load_feed(g_cfg.feed_path);
                if (msgs.empty()) {
                    log::warn("StageSync: RESULT: SKIP nothing to feed (record a session first)");
                    g_feed = Feed::Done;
                    if (g_cfg.quit_after) quit_later();
                    return;
                }
                log::info("StageSync: test feed: sending {} recorded branches as the host's", msgs.size());
                out.insert(out.end(), msgs.begin(), msgs.end());
                g_feed = Feed::Sent;
                g_feed_ms = now_ms;
            }
            break;
        case Feed::Sent:
            if (c.applied > 0) {
                log::info("StageSync: RESULT: PASS {} branch(es) from the network applied to this world's stages",
                          c.applied);
                g_feed = Feed::Applied;
                g_applied_ms = now_ms;
            } else if (now_ms - g_feed_ms > kFeedGiveUpMs) {
                log::warn("StageSync: RESULT: {} nothing applied in {} min ({})",
                          c.received ? "FAIL" : "SKIP", kFeedGiveUpMs / 60000, stage_sync_describe());
                g_feed = Feed::Done;
                if (g_cfg.quit_after) quit_later();
            }
            break;
        case Feed::Applied:
            if (now_ms - g_applied_ms >= kSurviveMs) {
                log::info("StageSync: RESULT: PASS game still running {} s after applying ({})", kSurviveMs / 1000,
                          stage_sync_describe());
                g_feed = Feed::Done;
                if (g_cfg.quit_after) quit_later();
            }
            break;
        case Feed::Done:
            break;
    }
}

}  // namespace

void stage_sync_configure(const StageSyncSettings& s) {
    g_cfg = s;
    if (s.enabled) {
        log::warn("Stage sync: ON (experimental: the host's quest stage branches are applied on the guest){}",
                  s.feed_path.empty() ? "" : " | test feed from cdmp_stage_record.txt");
    }
}

std::vector<proto::StageBranchMsg> stage_sync_tick(bool linked, bool host, bool echo, bool in_world,
                                                   uint32_t now_ms) {
    std::vector<proto::StageBranchMsg> out;
    if (!g_cfg.enabled) return out;
    // A loading screen: stage instance ids are reused in the next world.
    const bool loading = sniffer_world_loading();
    if (loading && !g_was_loading) {
        // Runtime instance ids are reused in the next world; the queue names stages by static
        // key and stays (clearing it lost the host's branches: found by the fuzzer, tests/fuzz).
        stage_tracker_reset();
        g_in_world_since = 0;
    }
    g_was_loading = loading;
    if (in_world && !loading) {
        if (!g_in_world_since) g_in_world_since = now_ms ? now_ms : 1;
    } else {
        g_in_world_since = 0;
    }
    const bool send = linked && (host || echo);
    if (send != g_collecting) {
        stage_tracker_collect(send);
        g_collecting = send;
    }
    if (send) {
        for (const auto& b : stage_tracker_take_game_branches()) out.push_back({b.key, b.branch, b.flags});
        feed_test(in_world && !loading, now_ms, out);
    }
    if (linked && (!host || echo) && in_world && !loading) {
        for (const auto& a : g_queue.tick(stage_tracker_local(), now_ms)) {
            const bool ok = stage_tracker_send_branch(a.instance, a.branch, a.flags);
            log::info("Stage sync: the host's branch {} of stage 0x{:X} -> our instance 0x{:X}{}", a.branch, a.key,
                      a.instance, ok ? "" : " (NOT sent: injection refused)");
        }
    }
    const auto& c = g_queue.counters();
    if (now_ms - g_last_stats >= 10000 && std::memcmp(&c, &g_logged, sizeof c) != 0) {
        log::info("Stage sync: {}", stage_sync_describe());
        g_logged = c;
        g_last_stats = now_ms;
    }
    return out;
}

void stage_sync_on_message(std::span<const uint8_t> bytes, uint32_t now_ms) {
    if (!g_cfg.enabled) return;
    const auto msg = proto::decode_stage_branch(bytes);
    if (!msg) {
        log::warn("Stage sync: malformed StageBranch message ({} bytes) ignored", bytes.size());
        return;
    }
    if (!g_queue.push(*msg, now_ms)) log::warn("Stage sync: queue full; branch of 0x{:X} dropped", msg->key);
}

std::string stage_sync_describe() {
    const auto& c = g_queue.counters();
    return std::format("received {} | sent to our server {} | applied {} | reached by our game alone {} | expired {} | "
                       "refused {} | pending {}",
                       c.received, c.sent, c.applied, c.reached_alone, c.expired, c.refused, g_queue.pending());
}

}  // namespace cdmp::net
