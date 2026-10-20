#pragma once
// Quest stages as this PC's game and server see them: runtime instance id ->
// static sequencer key (PrepareSequencerAck), state and branch
// (StageBranchStateAck), and the branches the game itself chooses
// (StageBranchStateReq). Fed by the sniffer; used by stage_sync and the dev
// tests. Formats: coop-anything games/crimson-desert/NOTES.md.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/mission_rows.h"
#include "common/stage_sync.h"

namespace cdmp::net {

struct StageInfo {
    uint64_t instance = 0;
    uint32_t key = 0;               // 0 = not known yet
    uint32_t state = 0, branch = 0xFFFFFFFF;
    uint64_t waiting_since = 0;     // first reported waiting (state 0, no branch)
    uint64_t game_req_ms = 0;       // the game's own last StageBranchStateReq for it
    uint64_t answer_ms = 0;         // last Ack carrying a branch
    uint32_t answer_branch = 0xFFFFFFFF;
};

// A branch the game itself chose, for a stage whose key is known.
struct GameBranch {
    uint32_t key = 0;
    uint32_t branch = 0;
    uint16_t flags = 0;
    uint64_t instance = 0;
};

// One quest in an UpdateQuestListAck delta (header a=0): a quest added or changed.
struct QuestDelta {
    uint32_t key = 0;
    uint8_t state = 0;
    uint8_t flag = 0;
};

// A quest dialog message the game itself sent (QuestDialogEndReq 2342 = the
// answer, SetQuestDialogObstacleReq 2621 = conversation open/closed), whole
// payload, while collecting. Accepting a quest goes this way, not by a stage branch.
struct DialogMsg {
    uint16_t op = 0;
    std::vector<uint8_t> payload;
    uint64_t ms = 0;  // GetTickCount64 when seen
};

// Sniffer writer thread (packets with their 5-byte header).
void stage_tracker_observe_s2c(const uint8_t* packet, size_t size);
void stage_tracker_observe_c2s(const uint8_t* packet, size_t size);

std::optional<StageInfo> stage_tracker_get(uint64_t instance);
std::vector<StageInfo> stage_tracker_all();
std::vector<LocalStage> stage_tracker_local();
// Branches the game chose since the last call (only while collecting is on).
void stage_tracker_collect(bool on);
std::vector<GameBranch> stage_tracker_take_game_branches();
// Sends StageBranchStateReq(instance, branch, flags) to our own server; the copy
// of it the sniffer sees is not counted as the game's own.
bool stage_tracker_send_branch(uint64_t instance, uint32_t branch, uint16_t flags);
// Quest list deltas since the last call.
std::vector<QuestDelta> stage_tracker_take_quest_deltas();
// Mission rows that appeared or changed since the last call (mission_rows.h).
std::vector<MissionChange> stage_tracker_take_mission_changes();
// Dialog messages the game sent since the last call (only while collecting is on).
std::vector<DialogMsg> stage_tracker_take_dialogs();
// Sends a dialog message (op 2342 or 2621, payload without header) to our own
// server; the copy the sniffer sees is not counted as the game's own.
bool stage_tracker_send_dialog(uint16_t op, const std::vector<uint8_t>& payload);
// NPCs as loaded (ActorLoadDataAck 2005: u32 runtime id at +3, u32 static character key
// at +22, e.g. Renee 0x01C29010). Runtime ids change between loads; static keys do not.
// 0 = not seen in this session.
uint32_t stage_tracker_actor_static(uint32_t runtime_id);
uint32_t stage_tracker_actor_runtime(uint32_t static_key);
// A new world (loading screen): instance ids are reused, forget them.
void stage_tracker_reset();

}  // namespace cdmp::net
