#include "mod/net/stage_replay.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

// Formats from the 2026-10-19 captures (coop-anything games/crimson-desert/NOTES.md).
constexpr uint16_t kOpStageBranchStateReq = 2436;   // u64 instance | u32 branch | u16 flags
constexpr uint16_t kOpStageBranchStateAck = 2981;   // hdr5 | u64 instance | u32 state | u32 branch | u32 hash ...
constexpr uint16_t kOpPrepareSequencerAck = 2661;   // hdr5 | u32 player | u64 instance | u16 | u32 sequencer key ...
constexpr uint32_t kNoBranch = 0xFFFFFFFF;
constexpr uint64_t kReplayAfterMs = 2000;   // the game's own choice came ~45 s after the stage appeared
constexpr uint64_t kAnswerWaitMs = 3000;
constexpr uint64_t kSurviveMs = 30000;
constexpr uint64_t kReplayGiveUpMs = 6 * 60 * 1000;

struct Instance {
    uint32_t key = 0;               // sequencer key (static), 0 = not seen yet
    uint32_t state = 0, branch = kNoBranch;
    uint64_t waiting_since = 0;     // reported state 0 / no branch
    uint64_t game_req_ms = 0;       // the game sent its own StageBranchStateReq
    uint64_t answer_ms = 0;         // last Ack with a branch
    uint32_t answer_branch = kNoBranch;
};

struct Recorded {
    uint32_t branch;
    uint16_t flags;
};

std::mutex g_mutex;
std::map<uint64_t, Instance> g_inst;
std::vector<std::string> g_lines;   // recorded lines waiting to be written (written by the loop thread)
bool g_record = false;
uint64_t g_ours = 0;                // our injected Req is captured too: skip one for this instance
int g_ours_left = 0;

template <class T>
T at(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

bool in_world() { return capture_own_player_key() != 0 && !sniffer_world_loading(); }

bool wait(void* stop, uint32_t ms) { return WaitForSingleObject(static_cast<HANDLE>(stop), ms) == WAIT_TIMEOUT; }

void flush_lines(const std::wstring& path) {
    std::vector<std::string> lines;
    {
        std::lock_guard lock(g_mutex);
        lines.swap(g_lines);
    }
    if (lines.empty()) return;
    std::ofstream out(path, std::ios::app);
    for (const auto& l : lines) out << l << '\n';
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

bool send_branch(uint64_t instance, uint32_t branch, uint16_t flags) {
    std::vector<uint8_t> p(5 + 14);
    const uint16_t op = kOpStageBranchStateReq, len = 14;
    std::memcpy(p.data(), &op, 2);
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, &instance, 8);
    std::memcpy(p.data() + 13, &branch, 4);
    std::memcpy(p.data() + 17, &flags, 2);
    {
        std::lock_guard lock(g_mutex);
        g_ours = instance;
        g_ours_left = 1;
    }
    return sniffer_inject_c2s(std::move(p));
}

void record_session(void* stop, const std::wstring& path, uint32_t stay_s) {
    DeleteFileW(path.c_str());
    {
        std::lock_guard lock(g_mutex);
        g_record = true;
    }
    log::info("StageReplay: recording the game's own stage branches for {} s in the world", stay_s);
    uint64_t start = 0;
    while (true) {
        if (!wait(stop, 1000)) return;
        if (!start && in_world()) start = GetTickCount64();
        flush_lines(path);
        if (start && GetTickCount64() - start >= stay_s * 1000ull) break;
    }
    {
        std::lock_guard lock(g_mutex);
        g_record = false;
    }
    flush_lines(path);
    const auto recs = load_records(path);
    log::info("StageReplay: RESULT: {} recorded {} stage keys", recs.empty() ? "SKIP" : "PASS", recs.size());
}

void replay_session(void* stop, const std::wstring& path) {
    const auto recs = load_records(path);
    if (recs.empty()) {
        log::warn("StageReplay: RESULT: SKIP nothing recorded in {}", std::string(path.begin(), path.end()));
        return;
    }
    log::info("StageReplay: replay of {} recorded stage keys", recs.size());
    const uint64_t start = GetTickCount64();
    uint64_t target = 0;
    Recorded rec{};
    uint32_t key = 0;
    while (!target) {
        if (!wait(stop, 200)) return;
        if (GetTickCount64() - start > kReplayGiveUpMs) {
            log::warn("StageReplay: RESULT: SKIP no recorded stage came up waiting in {} min", kReplayGiveUpMs / 60000);
            return;
        }
        if (!in_world()) continue;
        const uint64_t now = GetTickCount64();
        std::lock_guard lock(g_mutex);
        for (const auto& [instance, s] : g_inst) {
            const auto it = recs.find(s.key);
            if (s.key && it != recs.end() && s.state == 0 && s.branch == kNoBranch && s.waiting_since &&
                now - s.waiting_since >= kReplayAfterMs && !s.game_req_ms && !s.answer_ms) {
                target = instance;
                rec = it->second;
                key = s.key;
                break;
            }
        }
    }
    log::info("StageReplay: stage key 0x{:X} is instance 0x{:X} here; sending its recorded branch {} (flags {})", key,
              target, rec.branch, rec.flags);
    const uint64_t sent = GetTickCount64();
    if (!send_branch(target, rec.branch, rec.flags)) {
        log::error("StageReplay: RESULT: FAIL injection refused");
        return;
    }
    bool answered = false, game_first = false;
    uint32_t got = kNoBranch;
    while (GetTickCount64() - sent < kAnswerWaitMs && !answered) {
        if (!wait(stop, 50)) return;
        std::lock_guard lock(g_mutex);
        const Instance& s = g_inst[target];
        if (s.game_req_ms && s.game_req_ms >= sent && s.answer_ms <= s.game_req_ms) game_first = true;
        if (s.answer_ms >= sent) {
            answered = true;
            got = s.answer_branch;
        }
    }
    if (!answered) {
        log::error("StageReplay: RESULT: FAIL no answer for instance 0x{:X} in {} ms", target, kAnswerWaitMs);
    } else if (game_first) {
        log::warn("StageReplay: RESULT: SKIP the game sent its own request first: not attributable");
    } else if (got != rec.branch) {
        log::error("StageReplay: RESULT: FAIL instance 0x{:X} answered branch {}, expected {}", target, got, rec.branch);
    } else {
        log::info("StageReplay: RESULT: PASS instance 0x{:X} (key 0x{:X}) branched to {} {} ms after our request",
                  target, key, got, GetTickCount64() - sent);
    }
    // The game keeps running afterwards: a bad replay would crash it within seconds (as the 16:18 run did).
    if (!wait(stop, static_cast<uint32_t>(kSurviveMs))) return;
    log::info("StageReplay: RESULT: PASS game still running {} s after the replay", kSurviveMs / 1000);
}

}  // namespace

void stage_replay_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + 5 + 8) return;
    const uint16_t op = at<uint16_t>(packet);
    const uint8_t* p = packet + 5;
    const uint64_t now = GetTickCount64();
    if (op == kOpPrepareSequencerAck && size >= 5 + 23) {
        const uint64_t instance = at<uint64_t>(p + 9);
        std::lock_guard lock(g_mutex);
        Instance& s = g_inst[instance];
        if (!s.key) s.key = at<uint32_t>(p + 19);
    } else if (op == kOpStageBranchStateAck && size >= 5 + 25) {
        const uint64_t instance = at<uint64_t>(p + 5);
        std::lock_guard lock(g_mutex);
        Instance& s = g_inst[instance];
        s.state = at<uint32_t>(p + 13);
        s.branch = at<uint32_t>(p + 17);
        if (s.state == 0 && s.branch == kNoBranch) {
            if (!s.waiting_since) s.waiting_since = now;
        } else if (s.branch != kNoBranch) {
            s.answer_ms = now;
            s.answer_branch = s.branch;
        }
    }
}

void stage_replay_observe_c2s(const uint8_t* packet, size_t size) {
    if (size < 5 + 14 || at<uint16_t>(packet) != kOpStageBranchStateReq) return;
    const uint8_t* p = packet + 5;
    const uint64_t instance = at<uint64_t>(p);
    std::lock_guard lock(g_mutex);
    if (instance == g_ours && g_ours_left > 0) {
        --g_ours_left;
        return;
    }
    Instance& s = g_inst[instance];
    s.game_req_ms = GetTickCount64();
    if (g_record && s.key) {
        char line[64];
        std::snprintf(line, sizeof line, "%08X %u %u", s.key, at<uint32_t>(p + 8), at<uint16_t>(p + 12));
        g_lines.emplace_back(line);
    }
}

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
