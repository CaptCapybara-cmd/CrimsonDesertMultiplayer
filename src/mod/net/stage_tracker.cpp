#include "mod/net/stage_tracker.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>

#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {

constexpr uint16_t kOpStageBranchStateReq = 2436;  // u64 instance | u32 branch | u16 flags
constexpr uint16_t kOpStageBranchStateAck = 2981;  // hdr5 | u64 instance | u32 state | u32 branch | u32 hash ...
constexpr uint16_t kOpPrepareSequencerAck = 2661;  // hdr5 | u32 player | u64 instance | u16 | u32 sequencer key ...
constexpr uint16_t kOpUpdateQuestListAck = 2808;   // u8 scope | u32 player | u8 a | u8 b | 34-byte quests
constexpr size_t kQuestSize = 34;                  // u32 key | u32 | u64 | u64 | u64 | u8 state | u8 flag
constexpr size_t kMaxQuestDeltas = 512;
constexpr uint16_t kOpUpdateMissionListAck = 3009;   // u8 scope | u32 player | u8 a | u8 b | 55-byte missions
constexpr uint16_t kOpQuestDialogEndReq = 2342;      // u32 npc | u32 dialog key | u32 answer | ... (30 bytes)
constexpr uint16_t kOpSetQuestDialogObstacleReq = 2621;  // u32 actor | u8 on
constexpr size_t kMaxMissionChanges = 512;
constexpr size_t kMaxDialogs = 64;
constexpr uint32_t kNoBranch = 0xFFFFFFFF;
constexpr size_t kMaxInstances = 4096;
constexpr size_t kMaxGameBranches = 256;

std::mutex g_mutex;
std::map<uint64_t, StageInfo> g_stages;
std::vector<GameBranch> g_game_branches;
std::vector<QuestDelta> g_quest_deltas;
MissionRows g_missions;
std::vector<MissionChange> g_mission_changes;
std::vector<DialogMsg> g_dialogs;
int g_dialogs_ours = 0;
bool g_collect = false;
// Our own injected requests pass the same capture: one per instance is ours.
std::map<uint64_t, int> g_ours;

template <class T>
T at(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

StageInfo* entry(uint64_t instance) {
    auto it = g_stages.find(instance);
    if (it != g_stages.end()) return &it->second;
    if (g_stages.size() >= kMaxInstances) return nullptr;
    StageInfo& s = g_stages[instance];
    s.instance = instance;
    return &s;
}

}  // namespace

void stage_tracker_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + 5 + 8) return;
    const uint16_t op = at<uint16_t>(packet);
    const uint8_t* p = packet + 5;
    if (op == kOpUpdateMissionListAck) {
        std::lock_guard lock(g_mutex);
        for (const auto& c : g_missions.observe({p, size - 5})) {
            if (g_mission_changes.size() < kMaxMissionChanges) g_mission_changes.push_back(c);
        }
    } else if (op == kOpUpdateQuestListAck && size >= 5 + 7) {
        // Only deltas (a = 0): the full list at load (a = 1) is not news.
        if (p[5] != 0 || (size - 5 - 7) % kQuestSize != 0) return;
        std::lock_guard lock(g_mutex);
        for (size_t off = 7; off + kQuestSize <= size - 5 && g_quest_deltas.size() < kMaxQuestDeltas; off += kQuestSize) {
            g_quest_deltas.push_back({at<uint32_t>(p + off), p[off + 32], p[off + 33]});
        }
    } else if (op == kOpPrepareSequencerAck && size >= 5 + 23) {
        std::lock_guard lock(g_mutex);
        if (StageInfo* s = entry(at<uint64_t>(p + 9)); s && !s->key) s->key = at<uint32_t>(p + 19);
    } else if (op == kOpStageBranchStateAck && size >= 5 + 25) {
        const uint64_t now = GetTickCount64();
        std::lock_guard lock(g_mutex);
        StageInfo* s = entry(at<uint64_t>(p + 5));
        if (!s) return;
        s->state = at<uint32_t>(p + 13);
        s->branch = at<uint32_t>(p + 17);
        if (s->state == 0 && s->branch == kNoBranch) {
            if (!s->waiting_since) s->waiting_since = now;
        } else if (s->branch != kNoBranch) {
            s->answer_ms = now;
            s->answer_branch = s->branch;
        }
    }
}

void stage_tracker_observe_c2s(const uint8_t* packet, size_t size) {
    if (size >= 5 && (at<uint16_t>(packet) == kOpQuestDialogEndReq || at<uint16_t>(packet) == kOpSetQuestDialogObstacleReq)) {
        std::lock_guard lock(g_mutex);
        if (g_dialogs_ours > 0) {
            --g_dialogs_ours;
        } else if (g_collect && g_dialogs.size() < kMaxDialogs) {
            g_dialogs.push_back({at<uint16_t>(packet), {packet + 5, packet + size}, GetTickCount64()});
        }
        return;
    }
    if (size < 5 + 14 || at<uint16_t>(packet) != kOpStageBranchStateReq) return;
    const uint8_t* p = packet + 5;
    const uint64_t instance = at<uint64_t>(p);
    std::lock_guard lock(g_mutex);
    if (auto it = g_ours.find(instance); it != g_ours.end() && it->second > 0) {
        if (--it->second == 0) g_ours.erase(it);
        return;
    }
    StageInfo* s = entry(instance);
    if (!s) return;
    s->game_req_ms = GetTickCount64();
    if (g_collect && s->key && g_game_branches.size() < kMaxGameBranches) {
        g_game_branches.push_back({s->key, at<uint32_t>(p + 8), at<uint16_t>(p + 12), instance});
    }
}

std::optional<StageInfo> stage_tracker_get(uint64_t instance) {
    std::lock_guard lock(g_mutex);
    const auto it = g_stages.find(instance);
    if (it == g_stages.end()) return std::nullopt;
    return it->second;
}

std::vector<StageInfo> stage_tracker_all() {
    std::lock_guard lock(g_mutex);
    std::vector<StageInfo> out;
    out.reserve(g_stages.size());
    for (const auto& [_, s] : g_stages) out.push_back(s);
    return out;
}

std::vector<LocalStage> stage_tracker_local() {
    std::lock_guard lock(g_mutex);
    std::vector<LocalStage> out;
    out.reserve(g_stages.size());
    for (const auto& [_, s] : g_stages) {
        // The game's own request is out and its answer not seen yet (the capture lags a few ms).
        const bool busy = s.game_req_ms && s.game_req_ms > s.answer_ms;
        out.push_back({s.instance, s.key, s.state, s.branch, busy});
    }
    return out;
}

void stage_tracker_collect(bool on) {
    std::lock_guard lock(g_mutex);
    g_collect = on;
    if (!on) {
        g_game_branches.clear();
        g_dialogs.clear();
    }
}

std::vector<GameBranch> stage_tracker_take_game_branches() {
    std::lock_guard lock(g_mutex);
    std::vector<GameBranch> out;
    out.swap(g_game_branches);
    return out;
}

bool stage_tracker_send_branch(uint64_t instance, uint32_t branch, uint16_t flags) {
    std::vector<uint8_t> p(5 + 14);
    const uint16_t op = kOpStageBranchStateReq, len = 14;
    std::memcpy(p.data(), &op, 2);
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, &instance, 8);
    std::memcpy(p.data() + 13, &branch, 4);
    std::memcpy(p.data() + 17, &flags, 2);
    {
        std::lock_guard lock(g_mutex);
        ++g_ours[instance];
    }
    if (sniffer_inject_c2s(std::move(p))) return true;
    std::lock_guard lock(g_mutex);
    if (auto it = g_ours.find(instance); it != g_ours.end() && --it->second <= 0) g_ours.erase(it);
    return false;
}

std::vector<QuestDelta> stage_tracker_take_quest_deltas() {
    std::lock_guard lock(g_mutex);
    std::vector<QuestDelta> out;
    out.swap(g_quest_deltas);
    return out;
}

std::vector<MissionChange> stage_tracker_take_mission_changes() {
    std::lock_guard lock(g_mutex);
    std::vector<MissionChange> out;
    out.swap(g_mission_changes);
    return out;
}

std::vector<DialogMsg> stage_tracker_take_dialogs() {
    std::lock_guard lock(g_mutex);
    std::vector<DialogMsg> out;
    out.swap(g_dialogs);
    return out;
}

bool stage_tracker_send_dialog(uint16_t op, const std::vector<uint8_t>& payload) {
    if ((op != kOpQuestDialogEndReq && op != kOpSetQuestDialogObstacleReq) || payload.size() > 0xFFFF) return false;
    std::vector<uint8_t> p(5 + payload.size());
    const uint16_t len = static_cast<uint16_t>(payload.size());
    std::memcpy(p.data(), &op, 2);
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, payload.data(), payload.size());
    {
        std::lock_guard lock(g_mutex);
        ++g_dialogs_ours;
    }
    if (sniffer_inject_c2s(std::move(p))) return true;
    std::lock_guard lock(g_mutex);
    if (g_dialogs_ours > 0) --g_dialogs_ours;
    return false;
}

void stage_tracker_reset() {
    std::lock_guard lock(g_mutex);
    g_stages.clear();
    g_game_branches.clear();
    g_ours.clear();
}

}  // namespace cdmp::net
