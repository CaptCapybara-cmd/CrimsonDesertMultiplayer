#pragma once
// CORRECTION (NOTES.md 2026-10-20 15:15): what this calls a "template" is the game-clock
// time of the accept, not a request variant; there is no variant to force. Kept as a
// memory/hardware-watch diagnostic; do not use mode 3 in play.
// Dev diagnostic: where does the local server keep a mission slot, and what code
// writes a "request" quest's template into it at the accept (NOTES.md 2026-10-20)?
// Runs at QuestDialogEndReq's handler entry (quest_rng.h hook, server thread).
//   mode 1 (scan):  find every dword == slot key in private RW memory, snapshot
//                   +-256 bytes, compare 300 ms later; logs where a template-looking
//                   value (0x0D0000-0x0DFFFF) appeared, its offset from the key, and
//                   the offset of the slot's position float (signature).
//   mode 2 (watch): same scan; for hits whose signature matches, a hardware write
//                   watch (debug registers, server thread) on key + tmpl_off; each
//                   write logs the writing code (RIP) and exe return addresses on the stack.
//                   Found (2026-10-20): the server thread copies mission objects with
//                   exe+0x1A9B4A0 (operator=, rcx = dst, rdx = src): key at +0x28,
//                   kind/state +0x2C/+0x2D, template at +0x38.
//   mode 3 (force): hook that copy; whenever src's key is `key` or one of the
//                   `force_more` keys (the request's objective slots), every dword in
//                   src's first 0xD8 bytes that looks like a template (0x0D0000-0x0DFFFF)
//                   and differs from `force_template` is replaced by it first (the host's
//                   variant on the guest); the offsets changed are logged.
//   mode 4 (pool):  at the accept, find where several known variant templates of one
//                   request sit close together in readable memory (the pool the roll
//                   picks from): clusters, strides, a hex dump of the best one.
//   mode 5 (pool read watch): mode 4's scan, then hardware READ/WRITE watches on the
//                   template dword of 4 distinct variants' data records (the private
//                   allocation holding the most distinct variants); logs who reads them
//                   during the accept (the picker, if it looks at candidates).
// Test copy only.

#include <cstdint>

namespace cdmp::net {

struct MissionWatchSettings {
    int mode = 0;                 // 0 off, 1 scan, 2 watch, 3 force, 4 pool, 5 pool read watch
    uint32_t key = 0xFFFFF7F8;    // the slot
    uint32_t sig = 0xC626D4D4;    // a dword the slot's object holds (position x, -10677.2f)
    int sig_off = 0;              // mode 2: signature's offset from the key
    int tmpl_off = 0;             // mode 2: template field's offset from the key
    uint32_t force_template = 0;  // mode 3
    uint32_t force_more[4] = {};  // mode 3: more slot keys (0 = none)
};
void mission_watch_configure(const MissionWatchSettings& s);
// Server thread, at the accept's handler entry.
void mission_watch_on_accept();

}  // namespace cdmp::net
