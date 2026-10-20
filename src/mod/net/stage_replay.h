#pragma once
// Quest sync groundwork, dev only: record the stage branches the game itself
// chooses, then replay them on a fresh load by STATIC key. This is the shape
// quest sync needs: the host's branch, applied to the guest's own instance.
//
//   [dev] stage_record=1  every StageBranchStateReq the game sends is written to
//                         cdmp_stage_record.txt as "sequencer_key branch flags", and
//                         every quest list change as "Q quest_key state flag".
//   [dev] stage_replay=1  feeds all recorded branches through the stage sync queue
//                         (common/stage_sync.h) onto this session's instances, and
//                         checks that the recorded quest changes happen here too.
//                         Logs "StageReplay: RESULT: PASS|FAIL|SKIP".
// Only branches the game really sent are replayed: a made-up branch number
// crashes the game (NOTES.md 2026-10-19 16:18). Test copy only; never save.

#include <cstddef>
#include <cstdint>
#include <string>

namespace cdmp::net {

// Own thread. `dir` is the mod folder; `stay_s` = seconds in the world before
// a record session ends; `quit_after` closes the game when done. `stop` is a HANDLE.
void stage_replay_loop(void* stop, const std::wstring& dir, bool record, bool replay, uint32_t stay_s,
                       bool quit_after);

}  // namespace cdmp::net
