#include "mod/test_input.h"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cstring>
#include <vector>

#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp {
namespace {

// WM_INPUT lParam for a synthesized event: a tag no real HRAWINPUT has in its high
// half, with the event itself (key, down/up) in the low half, so queued events
// can be answered in any order.
constexpr uint64_t kMarkerTag = 0xCD3D0000ull << 32;
HRAWINPUT marker(uint16_t vk, bool down) {
    return reinterpret_cast<HRAWINPUT>(static_cast<uintptr_t>(kMarkerTag | (down ? 0x10000u : 0u) | vk));
}
bool is_marker(HRAWINPUT h) { return (reinterpret_cast<uintptr_t>(h) & 0xFFFFFFFF00000000ull) == kMarkerTag; }

SafetyHookInline g_get_raw_input_data;
SafetyHookInline g_get_async_key_state;
SafetyHookInline g_get_foreground_window;
SafetyHookInline g_get_focus;
SafetyHookInline g_register_raw_input;

std::atomic<HWND> g_raw_target{nullptr};  // hwndTarget the game registered for its keyboard
std::atomic<HWND> g_game_window{nullptr};
std::atomic<bool> g_fake_focus{false};
// The key held down right now (GetAsyncKeyState agrees with the events).
std::atomic<uint16_t> g_held_vk{0};
// Diagnostics: GetAsyncKeyState calls in all, and for the key we press.
std::atomic<uint32_t> g_async_calls{0};
std::atomic<uint32_t> g_async_calls_key{0};
std::atomic<uint16_t> g_press_vk{0};
// How often the game asked for the marker's data: proof a key reached its input code.
std::atomic<uint32_t> g_marker_reads{0};
// A real keyboard's handle: the game may drop events from a device it does not know.
std::atomic<HANDLE> g_keyboard{nullptr};

UINT WINAPI hook_get_raw_input_data(HRAWINPUT h, UINT command, LPVOID data, PUINT size, UINT header_size) {
    if (!is_marker(h)) return g_get_raw_input_data.stdcall<UINT>(h, command, data, size, header_size);
    if (const uint32_t n = g_marker_reads.fetch_add(1); n < 8) {
        log::info("TestInput: game read #{}: command 0x{:X} data {} size {}", n + 1, command, data ? "set" : "null",
                  size ? *size : 0);
    }
    if (!size || header_size != sizeof(RAWINPUTHEADER)) return static_cast<UINT>(-1);
    const UINT need = command == RID_HEADER ? sizeof(RAWINPUTHEADER) : sizeof(RAWINPUT);
    if (!data) {
        *size = need;
        return 0;
    }
    if (*size < need) {
        *size = need;
        return static_cast<UINT>(-1);
    }
    RAWINPUT ri{};
    ri.header.dwType = RIM_TYPEKEYBOARD;
    ri.header.dwSize = sizeof(RAWINPUT);
    ri.header.hDevice = g_keyboard.load();
    ri.header.wParam = RIM_INPUT;
    const auto bits = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
    const auto vk = static_cast<uint16_t>(bits & 0xFFFF);
    const bool down = (bits & 0x10000) != 0;
    ri.data.keyboard.MakeCode = static_cast<USHORT>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    ri.data.keyboard.Flags = down ? RI_KEY_MAKE : RI_KEY_BREAK;
    ri.data.keyboard.VKey = vk;
    ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
    std::memcpy(data, &ri, need);
    return need;
}

SHORT WINAPI hook_get_async_key_state(int vk) {
    g_async_calls.fetch_add(1, std::memory_order_relaxed);
    if (vk == g_press_vk.load(std::memory_order_relaxed)) g_async_calls_key.fetch_add(1, std::memory_order_relaxed);
    const uint16_t held = g_held_vk.load();
    if (held && vk == held) return static_cast<SHORT>(0x8000);
    return g_get_async_key_state.stdcall<SHORT>(vk);
}

HWND WINAPI hook_get_foreground_window() {
    if (g_fake_focus.load()) {
        if (HWND w = g_game_window.load()) return w;
    }
    return g_get_foreground_window.stdcall<HWND>();
}

HWND WINAPI hook_get_focus() {
    if (g_fake_focus.load()) {
        if (HWND w = g_game_window.load()) return w;
    }
    return g_get_focus.stdcall<HWND>();
}

BOOL WINAPI hook_register_raw_input(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    for (UINT i = 0; devices && i < count; ++i) {
        if (devices[i].usUsagePage == 1 && devices[i].usUsage == 6 && devices[i].hwndTarget) {  // keyboard
            g_raw_target.store(devices[i].hwndTarget);
        }
    }
    return g_register_raw_input.stdcall<BOOL>(devices, count, size);
}

// The game's own top-level window (visible, at least 640x360).
HWND find_game_window() {
    struct Ctx {
        DWORD pid;
        HWND hwnd;
    } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            RECT r{};
            if (pid == c->pid && IsWindowVisible(hwnd) && GetClientRect(hwnd, &r) && r.right - r.left >= 640 &&
                r.bottom - r.top >= 360) {
                c->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

template <class Fn>
bool hook(SafetyHookInline& h, HMODULE user32, const char* name, Fn detour) {
    auto* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
    if (!target) return false;
    h = safetyhook::create_inline(target, reinterpret_cast<void*>(detour));
    if (!h) log::error("TestInput: could not hook {}", name);
    return static_cast<bool>(h);
}

bool in_world() { return net::capture_own_player_key() != 0; }

}  // namespace

bool test_input_install() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return false;
    bool ok = hook(g_get_raw_input_data, user32, "GetRawInputData", &hook_get_raw_input_data);
    ok = hook(g_get_async_key_state, user32, "GetAsyncKeyState", &hook_get_async_key_state) && ok;
    ok = hook(g_get_foreground_window, user32, "GetForegroundWindow", &hook_get_foreground_window) && ok;
    ok = hook(g_get_focus, user32, "GetFocus", &hook_get_focus) && ok;
    ok = hook(g_register_raw_input, user32, "RegisterRawInputDevices", &hook_register_raw_input) && ok;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == 0 && count) {
        std::vector<RAWINPUTDEVICELIST> list(count);
        if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1)) {
            for (UINT i = 0; i < count; ++i) {
                if (list[i].dwType == RIM_TYPEKEYBOARD) {
                    g_keyboard.store(list[i].hDevice);
                    break;
                }
            }
        }
    }
    log::warn("TestInput: {} (keys are synthesized inside this game only; keyboard device {})",
              ok ? "installed" : "PARTLY installed", g_keyboard.load() ? "found" : "NOT found");
    return ok;
}

bool test_input_press(uint16_t vk, uint32_t hold_ms) {
    HWND game = g_game_window.load();
    if (!game) game = find_game_window();
    if (!game) return false;
    g_game_window.store(game);
    HWND target = g_raw_target.load();
    if (!target) target = game;
    g_press_vk.store(vk);
    // Queued like real input (PostMessage), so it goes through the game's own message loop.
    // Also the classic key messages, for UI layers that read those instead of Raw Input.
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const LPARAM down_lp = 1 | (static_cast<LPARAM>(scan) << 16);
    const LPARAM up_lp = down_lp | (1LL << 30) | (1LL << 31);
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    PostMessageW(target, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(marker(vk, true)));
    PostMessageW(game, WM_KEYDOWN, vk, down_lp);
    if (ch) PostMessageW(game, WM_CHAR, static_cast<WPARAM>(ch | 0x20), down_lp);  // lower case, as typed
    g_held_vk.store(vk);
    Sleep(hold_ms);
    g_held_vk.store(0);
    PostMessageW(target, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(marker(vk, false)));
    PostMessageW(game, WM_KEYUP, vk, up_lp);
    return true;
}

void test_autoload_loop(void* stop, uint16_t vk, uint32_t every_ms, int max_presses, bool quit_after) {
    const HANDLE h = static_cast<HANDLE>(stop);
    while (!find_game_window()) {
        if (WaitForSingleObject(h, 500) != WAIT_TIMEOUT) return;
    }
    // Let the intro and title screen settle before the first key (and give a person time to focus the window).
    if (WaitForSingleObject(h, 30000) != WAIT_TIMEOUT) return;
    g_fake_focus.store(true);
    int presses = 0;
    while (!in_world() && !net::sniffer_world_loading() && presses < max_presses) {
        ++presses;
        const bool ok = test_input_press(vk);
        log::info("TestInput: autoload press {} of key 0x{:02X} {} (input reads {}, GetAsyncKeyState {} / for the key {}, raw target {})",
                  presses, vk, ok ? "sent" : "NOT sent (no window)", g_marker_reads.load(), g_async_calls.load(),
                  g_async_calls_key.load(), g_raw_target.load() ? "registered window" : "game window");
        if (WaitForSingleObject(h, every_ms) != WAIT_TIMEOUT) break;
    }
    g_fake_focus.store(false);
    if (in_world() || net::sniffer_world_loading()) {
        log::info("TestInput: autoload done after {} presses (world {})", presses, in_world() ? "entered" : "loading");
    } else {
        log::error("TestInput: autoload gave up after {} presses: still not loading", presses);
    }
    if (quit_after) {
        // Autoload check only: stay a little in the world, then close.
        for (int i = 0; i < 120 && !in_world(); ++i) {
            if (WaitForSingleObject(h, 1000) != WAIT_TIMEOUT) return;
        }
        log::info("TestInput: RESULT: {} in the world {} (presses {}, input reads {})", in_world() ? "PASS" : "FAIL",
                  in_world() ? "entered" : "not entered", presses, g_marker_reads.load());
        if (WaitForSingleObject(h, 10000) == WAIT_TIMEOUT) test_quit_game(20000);
    }
}

void test_quit_game(uint32_t grace_ms) {
    log::warn("TestInput: closing the game without saving");
    if (HWND game = find_game_window()) PostMessageW(game, WM_CLOSE, 0, 0);
    Sleep(grace_ms);
    log::warn("TestInput: still running after {} ms; hard exit", grace_ms);
    TerminateProcess(GetCurrentProcess(), 0);
}

}  // namespace cdmp
