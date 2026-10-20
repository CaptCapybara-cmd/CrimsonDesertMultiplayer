#pragma once
// The local server's random draws for quest dialog answers, build 2976 (NOTES.md
// 2026-10-20). The server's rand is per thread (MSVC style, x*0x343FD+0x269EC3,
// state at TLS+0x4AD8, seeded with GetCurrentThreadId; getter exe+0x1378F10). The
// accept's chance gates and weighted picks (0x22CF730 / 0x22CFDD0) draw from it.
// Hooking QuestDialogEndReq's handler (exe+0x2A4C510, on the server thread) reads
// and sets the state those draws start from. The idea was: the same state on
// another PC gives the same "request" variant.
// Result (NOTES.md 2026-10-20 13:20-13:50): pinning this state does NOT fix the
// variant; the server thread's draws were identical across runs while the variant
// differed, so the variant comes from somewhere else. Kept as a diagnostic.
// Experimental, dev only.

#include <cstdint>

namespace cdmp::net {

// Hooks the handler; every accept then logs the RNG state it starts from. `trace`: also hook
// the rand getter (hot) and two candidates, and log every draw for 3 s after a pinned answer.
bool quest_rng_install(bool trace);
// The next QuestDialogEndReq the server handles starts from this state.
void quest_rng_pin_next(uint32_t state);
// The state the last handled QuestDialogEndReq started from (after a pin, the pinned value).
uint32_t quest_rng_last_state();

}  // namespace cdmp::net
