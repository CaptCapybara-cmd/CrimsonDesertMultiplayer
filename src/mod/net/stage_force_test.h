#pragma once
// Quest sync groundwork, dev only ([dev] stage_force_test=1): can a request we
// inject branch a waiting quest stage? [dev] stage_force_opcode picks the request:
// 2436 StageBranchStateReq (the game's own, u64 instance | u32 branch | u16) or
// 2476 StageChartForceBranchDevReq (u64 | u32; a stub in retail 1.0.0.2976, so it
// serves as the mutation run that must FAIL).
// It WRITES to the game's quest state: test copy only, never save afterwards.
//
// Automatic, no keys. Once in the world:
//   1. negative control: force a branch on an instance that does not exist;
//      the server must not answer for it;
//   2. the real test: the first stage the server reports as waiting
//      (StageBranchStateAck state 0, branch FFFFFFFF) is forced to branch 0;
//      the server must answer like it does for the client's own
//      StageBranchStateReq (Ack state 1 branch 0, or StageOnBranchEventAck).
// Each step logs "StageTest: RESULT: PASS|FAIL|SKIP ...".

#include <cstddef>
#include <cstdint>

namespace cdmp::net {

// Sniffer writer thread (packets with their 5-byte header).
void stage_test_observe_s2c(const uint8_t* packet, size_t size);
void stage_test_observe_c2s(const uint8_t* packet, size_t size);
// Own thread; `stop` is a HANDLE.
// With `quit_after`, closes the game without saving once the test is over.
void stage_test_loop(void* stop, uint16_t opcode, bool quit_after);

}  // namespace cdmp::net
