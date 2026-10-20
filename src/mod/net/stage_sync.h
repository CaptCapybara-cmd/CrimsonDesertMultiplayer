#pragma once
// Quest stage sync, experimental ([coop] stage_sync=1): the HOST sends every
// quest stage branch its game chooses (StageBranchStateReq), named by the
// stage's static sequencer key; the GUEST applies it to its own instance of
// that stage when the stage is waiting there (common/stage_sync.h has the
// rules). Through cdmp_peer one PC is both, so its own branches come back
// already reached; [dev] stage_sync_feed=1 also sends the first branch of
// each key in cdmp_stage_record.txt (stage_replay.h) as if a host had chosen
// it, which the guest half must then apply. Test copy only; never save.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/protocol.h"

namespace cdmp::net {

struct StageSyncSettings {
    bool enabled = false;
    std::wstring feed_path;   // non-empty: echo test feed from this record file
    bool quit_after = false;  // with a feed: close the game once the test is decided
};
void stage_sync_configure(const StageSyncSettings& s);

// Co-op thread. `host`: we are the host (or talk to cdmp_peer, `echo`: both).
// Returns what to send to the other side now.
std::vector<proto::StageBranchMsg> stage_sync_tick(bool linked, bool host, bool echo, bool in_world,
                                                   uint32_t now_ms);
// Co-op thread: a StageBranch message from the other side.
void stage_sync_on_message(std::span<const uint8_t> bytes, uint32_t now_ms);
std::string stage_sync_describe();

}  // namespace cdmp::net
