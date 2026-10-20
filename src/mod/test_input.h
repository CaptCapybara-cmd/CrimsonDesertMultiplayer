#pragma once
// Unattended tests, dev only ([dev] test_autoload=1): the mod presses keys in
// its OWN game window so a test run gets from the title screen into the world
// with nobody at the keyboard, and closes the game when the test is done.
//
// Nothing reaches other applications: the game reads keys through Raw Input,
// so we post WM_INPUT with a marker handle to the game's own window and answer
// that handle in a GetRawInputData hook (GetAsyncKeyState agrees while the key
// is "down"). While autoload runs, GetForegroundWindow/GetFocus report the game
// window so an unfocused game still accepts the keys; that stops afterwards.

#include <cstdint>

namespace cdmp {

// Hooks user32 in this process. Call once, early. False if a hook failed.
bool test_input_install();
// Synthesized key press (down, hold, up) on the game window. Any thread.
bool test_input_press(uint16_t vk, uint32_t hold_ms = 120);
// Own thread: presses `vk` every `every_ms` until the world starts loading or we
// are in it (at most `max_presses`), then gives focus back. `stop` is a HANDLE.
// With `quit_after` (autoload check without a stage test), logs RESULT and closes the game once in the world.
void test_autoload_loop(void* stop, uint16_t vk, uint32_t every_ms, int max_presses, bool quit_after);
// Holds a key down (with auto-repeat) for hold_ms on the game window.
// `modifier_vk` (e.g. VK_SHIFT) is pressed first and released last.
bool test_input_hold(void* stop, uint16_t vk, uint32_t hold_ms, uint16_t modifier_vk = 0);
// Own thread ([dev] test_walk_s): once in the world, walks back and forth
// (W 4 s, S 4 s) for walk_s seconds, then logs the other player's copy's
// motion RESULT (puppet_motion.h) and, with quit_after, closes the game.
// With `run`, Shift is held too ([dev] test_walk_run).
void test_walk_loop(void* stop, uint32_t walk_s, bool run, bool quit_after);
// Ends the game without saving: WM_CLOSE to the game window, then a hard exit
// after `grace_ms` if it is still running.
void test_quit_game(uint32_t grace_ms);

}  // namespace cdmp
