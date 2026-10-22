#pragma once
// Flight recorder for the shared story ([sniffer] flight=1): the hub's cofr recorder
// (third_party/cofr) keeps the last 16 MB of what this PC sent, received, shared and
// applied, plus a digest of the shared quest state every 5 s. It is written to
// <mod dir>\flight\<label>-latest.cofr every 30 s (the game can end without running any
// shutdown code), and to a numbered file when Ctrl+F12 is pressed (the "now!" mark both
// players press together), when a shared accept fails here, and on a clean exit. With the files
// of both PCs, the hub's analyzer names the first point where they disagree:
//   python -I tools/flight/flight.py diverge host.cofr guest.cofr
// What is compared: for every stage key and mission row the host shared, its value on
// each PC (category 0: stage branches, 1: mission rows). Keys are static (sequencer and
// mission keys), so they match between PCs. A guest still waiting for an NPC differs
// on purpose until it applies the accept; the entry text says so.

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/protocol.h"

namespace cdmp::net {

void flight_configure(bool enabled, const std::wstring& dir, bool host, const std::string& label);
bool flight_enabled();
void flight_net_out(const uint8_t* data, size_t size);
void flight_net_in(const uint8_t* data, size_t size);
// Host: an entry went into its story log. Guest: an entry was applied (new) here.
void flight_story_captured(const proto::StoryEntry& e);
void flight_story_applied(const proto::StoryEntry& e);
// Co-op thread, often: a digest of the shared state every 5 s. `last_seq`: the newest story
// entry this PC has (host: logged, guest: applied).
void flight_tick(uint32_t now_ms, uint32_t log_id, uint32_t last_seq);
void flight_marker(const std::string& text);
// Writes a file now (Ctrl+F12, shutdown) or for a problem (rate limited).
void flight_dump(const char* why);
void flight_desync(const std::string& what);

}  // namespace cdmp::net
