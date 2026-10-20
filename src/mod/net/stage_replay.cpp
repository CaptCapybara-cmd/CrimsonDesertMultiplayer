#include "mod/net/stage_replay.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/stage_tracker.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

constexpr uint32_t kNoBranch = 0xFFFFFFFF;
constexpr uint64_t kReplayAfterMs = 2000;   // the game's own choice came ~45 s after the stage appeared
constexpr uint64_t kAnswerWaitMs = 3000;
constexpr uint64_t kSurviveMs = 30000;
constexpr uint64_t kReplayGiveUpMs = 6 * 60 * 1000;

struct Recorded {
    uint32_t branch;
    uint16_t flags;
};

bool in_world() { return capture_own_player_key() != 0 && !sniffer_world_loading(); }

bool wait(void* stop, uint32_t ms) { return WaitForSingleObject(static_cast<HANDLE>(stop), ms) == WAIT_TIMEOUT; }

void append_lines(const std::wstring& path, const std::vector<GameBranch>& branches) {
    if (branches.empty()) return;
    std::ofstream out(path, std::ios::app);
    for (const auto& b : branches) {
        char line[64];
        std::snprintf(line, sizeof line, "%08X %u %u", b.key, b.branch, b.flags);
        out << line << '\n';
    }
}

std::map<uint32_t, Recorded> load_records(const std::wstring& path) {
    std::map<uint32_t, Recorded> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        unsigned key = 0, branch = 0, flags = 0;
        if (std::sscanf(line.c_str(), "%x %u %u", &key, &branch, &flags) == 3 && !out.contains(key)) {
            out[key] = {branch, static_cast<uint16_t>(flags)};  // the first branch the game chose for this stage
        }
    }
    return out;
}

void record_session(void* stop, const std::wstring& path, uint32_t stay_s) {
    DeleteFileW(path.c_str());
    stage_tracker_collect(true);
    log::info("StageReplay: recording the game's own stage branches for {} s in the world", stay_s);
    const uint64_t start = GetTickCount64();
    while (GetTickCount64() - start < stay_s * 1000ull) {
        if (!wait(stop, 1000)) return;
        append_lines(path, stage_tracker_take_game_branches());
    }
    append_lines(path, stage_tracker_take_game_branches());
    stage_tracker_collect(false);
    const auto recs = load_records(path);
    log::info("StageReplay: RESULT: {} recorded {} stage keys", recs.empty() ? "SKIP" : "PASS", recs.size());
}

void replay_session(void* stop, const std::wstring& path) {
    const auto recs = load_records(path);
    if (recs.empty()) {
        log::warn("StageReplay: RESULT: SKIP nothing recorded");
        return;
    }
    log::info("StageReplay: replay of {} recorded stage keys", recs.size());
    const uint64_t start = GetTickCount64();
    StageInfo target;
    Recorded rec{};
    while (!target.instance) {
        if (!wait(stop, 200)) return;
        if (GetTickCount64() - start > kReplayGiveUpMs) {
            log::warn("StageReplay: RESULT: SKIP no recorded stage came up waiting in {} min", kReplayGiveUpMs / 60000);
            return;
        }
        if (!in_world()) continue;
        const uint64_t now = GetTickCount64();
        for (const auto& s : stage_tracker_all()) {
            const auto it = recs.find(s.key);
            if (s.key && it != recs.end() && s.state == 0 && s.branch == kNoBranch && s.waiting_since &&
                now - s.waiting_since >= kReplayAfterMs && !s.game_req_ms && !s.answer_ms) {
                target = s;
                rec = it->second;
                break;
            }
        }
    }
    log::info("StageReplay: stage key 0x{:X} is instance 0x{:X} here; sending its recorded branch {} (flags {})",
              target.key, target.instance, rec.branch, rec.flags);
    const uint64_t sent = GetTickCount64();
    if (!stage_tracker_send_branch(target.instance, rec.branch, rec.flags)) {
        log::error("StageReplay: RESULT: FAIL injection refused");
        return;
    }
    std::optional<StageInfo> s;
    bool answered = false, game_first = false;
    while (GetTickCount64() - sent < kAnswerWaitMs && !answered) {
        if (!wait(stop, 50)) return;
        s = stage_tracker_get(target.instance);
        if (!s) continue;
        if (s->game_req_ms >= sent && s->answer_ms <= s->game_req_ms) game_first = true;
        answered = s->answer_ms >= sent;
    }
    if (!answered) {
        log::error("StageReplay: RESULT: FAIL no answer for instance 0x{:X} in {} ms", target.instance, kAnswerWaitMs);
    } else if (game_first) {
        log::warn("StageReplay: RESULT: SKIP the game sent its own request first: not attributable");
    } else if (s->answer_branch != rec.branch) {
        log::error("StageReplay: RESULT: FAIL instance 0x{:X} answered branch {}, expected {}", target.instance,
                   s->answer_branch, rec.branch);
    } else {
        log::info("StageReplay: RESULT: PASS instance 0x{:X} (key 0x{:X}) branched to {} {} ms after our request",
                  target.instance, target.key, s->answer_branch, GetTickCount64() - sent);
    }
    // The game keeps running afterwards: a bad replay would crash it within seconds (as the 16:18 run did).
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
