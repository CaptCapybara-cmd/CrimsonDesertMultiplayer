#pragma once
// Quest accept sharing, experimental ([coop] quest_share=1): when the HOST accepts a
// quest from an NPC (a request), the GUEST's mod gives its own server the same
// accept, so both players have the same quest; each then completes it alone (rules
// in common/quest_accept.h). The NPC's meeting scene reaches the guest through
// stage sync ([coop] stage_sync=1), which should be on too.
// Through cdmp_peer one PC is both: its own accepts come back already done; [dev]
// quest_share_feed=1 also sends the accept in cdmp_stage_record.txt ("D 2342 ...",
// "N ..." and "S key before after" lines) as if a host had made it, which the guest
// half must then apply. Test copy only; never save.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "common/protocol.h"

namespace cdmp::net {

struct QuestShareSettings {
    bool enabled = false;
    std::wstring feed_path;   // non-empty: echo test feed from this record file
    bool quit_after = false;  // with a feed: close the game once the test is decided
};
void quest_share_configure(const QuestShareSettings& s);

// Co-op thread. Returns what to send to the other side now.
std::vector<proto::QuestAcceptMsg> quest_share_tick(bool linked, bool host, bool echo, bool in_world);
// Co-op thread: a QuestAccept message from the other side.
void quest_share_on_message(std::span<const uint8_t> bytes);
std::string quest_share_describe();

}  // namespace cdmp::net
