#pragma once
// Measures how the other player's copy actually moves on this screen, for the
// unattended walk test (test_input.h, [dev] test_walk_s): its position is read
// from the game about every 20 ms while a window is open, and two failures are
// counted:
//   jump             a step longer than 0.75 m and faster than 15 m/s;
//   stall then jump  the copy stood still >= 150 ms, then moved faster than
//                    10 m/s: the client's buffer overflowed and caught up
//                    (cdmp-puppet-findings.md, saltos-copia-anfitrion.md S4).
// The first 3 s of a window are not counted (the copy snaps to the sender after a rest).
// puppet_motion_report logs "Puppet motion: RESULT: PASS|FAIL|SKIP".

namespace cdmp::net {

void puppet_motion_window(bool on);     // any thread
void puppet_motion_sample();            // co-op thread, every tick
void puppet_motion_report(float walked_m);  // any thread, after the window

}  // namespace cdmp::net
