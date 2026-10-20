#include "mod/net/stage_replay.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "common/stage_sync.h"
#include "mod/net/stage_tracker.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

constexpr uint64_t kSurviveMs = 30000;
constexpr uint32_t kReplayGiveUpMs = 8 * 60 * 1000;

bool in_world() { return capture_own_player_key() != 0 && !sniffer_world_loading(); }

bool wait(void* stop, uint32_t ms) { return WaitForSingleObject(static_cast<HANDLE>(stop), ms) == WAIT_TIMEOUT; }

// cdmp_stage_record.txt: "KKKKKKKK branch flags" per branch the game chose, and
// "Q KKKKKKKK state flag" per quest list delta, in the order they happened.
struct Recording {
    std::vector<proto::StageBranchMsg> branches;
    std::map<uint32_t, uint8_t> quests;  // quest key -> last recorded state
};

void append(const std::wstring& path, const std::vector<GameBranch>& branches, const std::vector<QuestDelta>& quests) {
    if (branches.empty() && quests.empty()) return;
    std::ofstream out(path, std::ios::app);
    char line[64];
    for (const auto& b : branches) {
        std::snprintf(line, sizeof line, "%08X %u %u", b.key, b.branch, b.flags);
        out << line << '\n';
    }
    for (const auto& q : quests) {
        std::snprintf(line, sizeof line, "Q %08X %u %u", q.key, q.state, q.flag);
        out << line << '\n';
        log::info("StageReplay: quest 0x{:X} now state {} (flag {})", q.key, q.state, q.flag);
    }
}

Recording load(const std::wstring& path) {
    Recording r;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        unsigned key = 0, a = 0, b = 0;
        if (std::sscanf(line.c_str(), "Q %x %u %u", &key, &a, &b) == 3) {
            r.quests[key] = static_cast<uint8_t>(a);
        } else if (std::sscanf(line.c_str(), "%x %u %u", &key, &a, &b) == 3 && key) {
            r.branches.push_back({key, a, static_cast<uint16_t>(b)});
        }
    }
    return r;
}

void record_session(void* stop, const std::wstring& path, uint32_t stay_s) {
    DeleteFileW(path.c_str());
    stage_tracker_collect(true);
    stage_tracker_take_quest_deltas();
    log::info("StageReplay: recording the game's own stage branches and quest changes for {} s in the world", stay_s);
    const uint64_t start = GetTickCount64();
    while (GetTickCount64() - start < stay_s * 1000ull) {
        const bool go_on = wait(stop, 1000);
        append(path, stage_tracker_take_game_branches(), stage_tracker_take_quest_deltas());
        if (!go_on) return;
    }
    stage_tracker_collect(false);
    const auto r = load(path);
    log::info("StageReplay: RESULT: {} recorded {} branches, {} quest changes", r.branches.empty() ? "SKIP" : "PASS",
              r.branches.size(), r.quests.size());
}

// Every recorded branch goes through the same queue the network sync uses; the
// recorded quest changes are the oracle: they must come back here too.
void replay_session(void* stop, const std::wstring& path) {
    const auto rec = load(path);
    if (rec.branches.empty()) {
        log::warn("StageReplay: RESULT: SKIP nothing recorded");
        return;
    }
    log::info("StageReplay: replay of {} recorded branches; expecting {} quest changes", rec.branches.size(),
              rec.quests.size());
    StageSyncQueue queue(StageSyncQueue::Settings{kReplayGiveUpMs, 5000, 1024});
    const uint64_t t0 = GetTickCount64();
    for (const auto& b : rec.branches) queue.push(b, static_cast<uint32_t>(t0));
    stage_tracker_take_quest_deltas();
    std::map<uint32_t, uint8_t> seen;
    uint64_t last_progress = t0;
    uint32_t last_applied = 0;
    auto matched = [&] {
        size_t n = 0;
        for (const auto& [k, st] : rec.quests) n += seen.contains(k) && seen.at(k) == st;
        return n;
    };
    while (GetTickCount64() - t0 < kReplayGiveUpMs) {
        if (!wait(stop, 200)) return;
        if (!in_world()) continue;
        const uint32_t now = static_cast<uint32_t>(GetTickCount64());
        for (const auto& a : queue.tick(stage_tracker_local(), now)) {
            const bool ok = stage_tracker_send_branch(a.instance, a.branch, a.flags);
            log::info("StageReplay: branch {} of stage 0x{:X} -> instance 0x{:X}{}", a.branch, a.key, a.instance,
                      ok ? "" : " (NOT sent)");
        }
        for (const auto& q : stage_tracker_take_quest_deltas()) {
            seen[q.key] = q.state;
            log::info("StageReplay: quest 0x{:X} now state {}{}", q.key, q.state,
                      rec.quests.contains(q.key) ? " (recorded)" : "");
        }
        if (queue.counters().applied != last_applied) {
            last_applied = queue.counters().applied;
            last_progress = GetTickCount64();
        }
        // Done: every recorded quest change came back, or nothing moved for 90 s after the last apply.
        if (!rec.quests.empty() && matched() == rec.quests.size()) break;
        if (last_applied && GetTickCount64() - last_progress > 90000) break;
    }
    const auto& c = queue.counters();
    const size_t m = matched();
    log::info("StageReplay: branches applied {} | reached alone {} | expired {} | still pending {}", c.applied,
              c.reached_alone, c.expired, queue.pending());
    if (rec.quests.empty()) {
        log::info("StageReplay: RESULT: {} {} branches applied (no quest change was recorded to compare)",
                  c.applied ? "PASS" : "FAIL", c.applied);
    } else if (m == rec.quests.size()) {
        log::info("StageReplay: RESULT: PASS all {} recorded quest changes happened here too", m);
    } else {
        log::error("StageReplay: RESULT: FAIL {} of {} recorded quest changes happened here", m, rec.quests.size());
    }
    if (!wait(stop, static_cast<uint32_t>(kSurviveMs))) return;
    log::info("StageReplay: RESULT: PASS game still running {} s after the replay", kSurviveMs / 1000);
}

}  // namespace

void stage_replay_loop(void* stop, const std::wstring& dir, bool record, bool replay, uint32_t stay_s,
                       bool quit_after) {
    const std::wstring path = dir + L"\\cdmp_stage_record.txt";
    log::warn("StageReplay: {} (test copy only; do not save)", record ? "RECORD" : "REPLAY, it WRITES quest state");
    while (!in_world()) {
        if (!wait(stop, 500)) return;
    }
    if (record) record_session(stop, path, stay_s);
    else if (replay) replay_session(stop, path);
    if (quit_after && wait(stop, 3000)) test_quit_game(20000);
}

}  // namespace cdmp::net
