#include "mod/net/stage_force_test.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

// Formats from the 2026-10-19 capture (coop-anything games/crimson-desert/NOTES.md).
constexpr uint16_t kOpStageBranchStateReq = 2436;  // u64 instance | u32 branch | u16
constexpr uint16_t kOpStageBranchStateAck = 2981;  // hdr5 | u64 instance | u32 state | u32 branch | u32 hash ...
constexpr uint16_t kOpStageOnBranchEventAck = 2524;  // hdr5 | u64 instance | u32 branch | u8
constexpr uint32_t kNoBranch = 0xFFFFFFFF;
constexpr uint64_t kBogusInstance = 0x7FFFFFF0;
constexpr uint64_t kAnswerWaitMs = 3000;
constexpr uint64_t kSettleMs = 2000;        // a waiting stage must stay waiting this long before we touch it
constexpr uint64_t kGiveUpMs = 15 * 60 * 1000;

struct Stage {
    uint32_t state = 0, branch = kNoBranch, hash = 0;
    uint64_t since_ms = 0;         // last time it was reported waiting
    uint64_t answer_ms = 0;        // last server answer (Ack branched or OnBranch)
    uint32_t answer_branch = kNoBranch;
    bool client_req = false;       // the game itself asked for a branch
};

std::mutex g_mutex;
std::map<uint64_t, Stage> g_stages;
// Our own injected StageBranchStateReq passes the same capture as the game's:
// one for this instance is ours, not the game's (under g_mutex).
uint64_t g_ours = 0;
int g_ours_left = 0;

template <class T>
T at(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// StageChartForceBranchDevReq: u64 | u32 (12 bytes). StageBranchStateReq, the
// game's own request: u64 | u32 | u16 (14 bytes, the u16 is 0 in every capture).
std::vector<uint8_t> force_packet(uint16_t op, uint64_t instance, uint32_t branch) {
    const uint16_t len = op == kOpStageBranchStateReq ? 14 : 12;
    std::vector<uint8_t> p(5 + len);
    std::memcpy(p.data(), &op, 2);
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, &instance, 8);
    std::memcpy(p.data() + 13, &branch, 4);
    return p;
}

// Waits for a server answer on `instance` after `sent_ms`; true if one came.
bool answered(uint64_t instance, uint64_t sent_ms, void* stop, uint32_t* branch, bool* client) {
    while (GetTickCount64() - sent_ms < kAnswerWaitMs) {
        if (WaitForSingleObject(static_cast<HANDLE>(stop), 50) != WAIT_TIMEOUT) return false;
        std::lock_guard lock(g_mutex);
        const auto it = g_stages.find(instance);
        if (it != g_stages.end() && it->second.answer_ms >= sent_ms) {
            *branch = it->second.answer_branch;
            *client = it->second.client_req;
            return true;
        }
    }
    return false;
}

bool in_world() { return capture_own_player_key() != 0 && !sniffer_world_loading(); }

bool send(uint16_t op, uint64_t instance, uint32_t branch) {
    {
        std::lock_guard lock(g_mutex);
        g_ours = instance;
        g_ours_left = 1;
    }
    return sniffer_inject_c2s(force_packet(op, instance, branch));
}

}  // namespace

void stage_test_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + 5 + 8 + 4) return;
    const uint16_t op = at<uint16_t>(packet);
    if (op != kOpStageBranchStateAck && op != kOpStageOnBranchEventAck) return;
    const uint8_t* p = packet + 5 + 5;  // past the packet header and the payload's scope + player key
    const uint64_t instance = at<uint64_t>(p);
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    Stage& s = g_stages[instance];
    if (op == kOpStageOnBranchEventAck) {
        s.answer_ms = now;
        s.answer_branch = at<uint32_t>(p + 8);
        return;
    }
    if (size < 5 + 5 + 8 + 12) return;
    s.state = at<uint32_t>(p + 8);
    s.branch = at<uint32_t>(p + 12);
    s.hash = at<uint32_t>(p + 16);
    if (s.state == 0 && s.branch == kNoBranch) {
        if (!s.since_ms) s.since_ms = now;
    } else if (s.branch != kNoBranch) {
        s.answer_ms = now;
        s.answer_branch = s.branch;
    }
}

void stage_test_observe_c2s(const uint8_t* packet, size_t size) {
    if (size < 5 + 8 || at<uint16_t>(packet) != kOpStageBranchStateReq) return;
    const uint64_t instance = at<uint64_t>(packet + 5);
    std::lock_guard lock(g_mutex);
    if (instance == g_ours && g_ours_left > 0) {
        --g_ours_left;
        return;
    }
    g_stages[instance].client_req = true;
}

namespace {

void run_test(void* stop, uint16_t opcode) {
    log::warn("StageTest: ON (opcode {}). It WRITES to the game's quest state: test copy only, do not save", opcode);
    const uint64_t start = GetTickCount64();
    while (!in_world()) {
        if (WaitForSingleObject(static_cast<HANDLE>(stop), 500) != WAIT_TIMEOUT) return;
    }
    log::info("StageTest: in the world");
    if (WaitForSingleObject(static_cast<HANDLE>(stop), 5000) != WAIT_TIMEOUT) return;

    // 1. Negative control.
    uint32_t branch = 0;
    bool client = false;
    uint64_t sent = GetTickCount64();
    if (!send(opcode, kBogusInstance, 0)) {
        log::error("StageTest: RESULT: FAIL negative control: injection refused");
        return;
    }
    if (answered(kBogusInstance, sent, stop, &branch, &client)) {
        log::error("StageTest: RESULT: FAIL negative control: the server answered for a made-up instance (branch {})", branch);
    } else {
        log::info("StageTest: RESULT: PASS negative control: no answer for made-up instance 0x{:X} in {} ms",
                  kBogusInstance, kAnswerWaitMs);
    }

    // 2. Real test: wait for a stage that has been waiting a while and the game left alone.
    uint64_t target = 0;
    Stage before;
    while (!target) {
        if (WaitForSingleObject(static_cast<HANDLE>(stop), 200) != WAIT_TIMEOUT) return;
        if (GetTickCount64() - start > kGiveUpMs) {
            log::warn("StageTest: RESULT: SKIP no waiting stage seen in {} min", kGiveUpMs / 60000);
            return;
        }
        if (!in_world()) continue;
        const uint64_t now = GetTickCount64();
        std::lock_guard lock(g_mutex);
        for (const auto& [instance, s] : g_stages) {
            if (instance != kBogusInstance && s.state == 0 && s.branch == kNoBranch && s.since_ms &&
                now - s.since_ms >= kSettleMs && !s.client_req && !s.answer_ms) {
                target = instance;
                before = s;
                break;
            }
        }
    }
    log::info("StageTest: target stage 0x{:X} (hash 0x{:08X}, waiting {} ms); forcing branch 0", target, before.hash,
              GetTickCount64() - before.since_ms);
    sent = GetTickCount64();
    if (!send(opcode, target, 0)) {
        log::error("StageTest: RESULT: FAIL injection refused");
        return;
    }
    if (!answered(target, sent, stop, &branch, &client)) {
        log::error("StageTest: RESULT: FAIL no answer for stage 0x{:X} in {} ms", target, kAnswerWaitMs);
    } else if (client) {
        log::warn("StageTest: RESULT: SKIP stage 0x{:X} answered (branch {}) but the game also sent its own "
                  "StageBranchStateReq: not attributable", target, branch);
    } else {
        log::info("StageTest: RESULT: PASS stage 0x{:X} branched to {} {} ms after our request", target, branch,
                  GetTickCount64() - sent);
    }
}

}  // namespace

void stage_test_loop(void* stop, uint16_t opcode, bool quit_after) {
    run_test(stop, opcode);
    if (quit_after && WaitForSingleObject(static_cast<HANDLE>(stop), 5000) == WAIT_TIMEOUT) {
        log::info("StageTest: done; closing the game (test_autoquit)");
        test_quit_game(20000);
    }
}

}  // namespace cdmp::net
