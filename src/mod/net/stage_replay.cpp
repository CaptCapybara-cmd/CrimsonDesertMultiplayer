#include "mod/net/stage_replay.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/mission_watch.h"
#include "mod/net/quest_rng.h"
#include "common/stage_sync.h"
#include "mod/net/stage_tracker.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

constexpr uint64_t kSurviveMs = 30000;
constexpr uint32_t kReplayGiveUpMs = 8 * 60 * 1000;

bool in_world() { return capture_own_player_key() != 0 && !sniffer_world_loading(); }

bool wait(void* stop, uint32_t ms) { return WaitForSingleObject(static_cast<HANDLE>(stop), ms) == WAIT_TIMEOUT; }

constexpr uint32_t kDialogAfterMs = 20000;  // in the world this long before the recorded dialog goes out
constexpr uint64_t kDialogGapMaxMs = 3000;
constexpr uint64_t kNpcWaitMs = 60000;

// cdmp_stage_record.txt, in the order things happened:
//   "KKKKKKKK branch flags"      a branch the game chose
//   "Q KKKKKKKK state flag"      a quest list delta
//   "M KKKKKKKK kind state hex"  a mission row that appeared or changed (hex = the whole 55-byte row;
//                                the accept time in it, +14 in a kind-5 row else +6, is not compared)
//   "D op ms hex"                a quest dialog message the game sent (ms = since the record started)
//   "N RRRRRRRR SSSSSSSS"        an NPC in those messages: runtime id -> static character key
//                                (runtime ids change between loads; the replay translates them)
struct Recording {
    std::vector<proto::StageBranchMsg> branches;
    std::map<uint32_t, uint8_t> quests;  // quest key -> last recorded state
    std::map<uint32_t, std::vector<uint8_t>> missions;  // mission key -> last whole row
    struct Dialog {
        uint16_t op;
        uint64_t ms;
        std::vector<uint8_t> payload;
    };
    std::vector<Dialog> dialogs;
    std::map<uint32_t, uint32_t> npcs;  // recorded runtime id -> static key
};

bool is_npc_id(uint32_t id) { return (id >> 24) == 0xB0; }

std::string hex(const std::vector<uint8_t>& b) {
    std::string s;
    char h[3];
    for (uint8_t v : b) {
        std::snprintf(h, sizeof h, "%02X", v);
        s += h;
    }
    return s;
}

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        unsigned v = 0;
        if (std::sscanf(s.c_str() + i, "%2x", &v) != 1) return {};
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

void append(const std::wstring& path, const std::vector<GameBranch>& branches, const std::vector<QuestDelta>& quests,
            const std::vector<MissionChange>& missions = {}, const std::vector<DialogMsg>& dialogs = {},
            uint64_t start = 0) {
    if (branches.empty() && quests.empty() && missions.empty() && dialogs.empty()) return;
    std::ofstream out(path, std::ios::app);
    char line[64];
    for (const auto& d : dialogs) {
        out << "D " << d.op << ' ' << (d.ms > start ? d.ms - start : 0) << ' ' << hex(d.payload) << '\n';
        log::info("StageReplay: dialog message {} ({} bytes)", d.op, d.payload.size());
        uint32_t id = 0;
        if (d.payload.size() >= 4) std::memcpy(&id, d.payload.data(), 4);
        if (is_npc_id(id)) {
            const uint32_t key = stage_tracker_actor_static(id);
            std::snprintf(line, sizeof line, "N %08X %08X", id, key);
            out << line << '\n';
            if (!key) log::warn("StageReplay: NPC 0x{:X} in a dialog has no known static key", id);
        }
    }
    for (const auto& m : missions) {
        std::snprintf(line, sizeof line, "M %08X %u %u ", m.key, m.kind, m.state);
        out << line << hex({m.row.begin(), m.row.end()}) << '\n';
        log::info("StageReplay: mission 0x{:X} {} kind {} state {}", m.key, m.added ? "added" : "changed", m.kind,
                  m.state);
    }
    for (const auto& b : branches) {
        std::snprintf(line, sizeof line, "%08X %u %u", b.key, b.branch, b.flags);
        out << line << '\n';
    }
    for (const auto& q : quests) {
        std::snprintf(line, sizeof line, "Q %08X %u %u", q.key, q.state, q.flag);
        out << line << '\n';
        log::info("StageReplay: quest 0x{:X} now state {} (flag {})", q.key, q.state, q.flag);
    }
}

Recording load(const std::wstring& path) {
    Recording r;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        unsigned key = 0, a = 0, b = 0;
        unsigned long long ms = 0;
        char buf[4096];
        if (std::sscanf(line.c_str(), "D %u %llu %4095s", &a, &ms, buf) == 3) {
            auto payload = unhex(buf);
            if (!payload.empty()) r.dialogs.push_back({static_cast<uint16_t>(a), ms, std::move(payload)});
        } else if (std::sscanf(line.c_str(), "N %x %x", &key, &a) == 2) {
            if (a) r.npcs[key] = a;
        } else if (std::sscanf(line.c_str(), "M %x %u %u %4095s", &key, &a, &b, buf) == 4) {
            r.missions[key] = unhex(buf);
        } else if (std::sscanf(line.c_str(), "Q %x %u %u", &key, &a, &b) == 3) {
            r.quests[key] = static_cast<uint8_t>(a);
        } else if (std::sscanf(line.c_str(), "%x %u %u", &key, &a, &b) == 3 && key) {
            r.branches.push_back({key, a, static_cast<uint16_t>(b)});
        }
    }
    return r;
}

void record_session(void* stop, const std::wstring& path, uint32_t stay_s) {
    DeleteFileW(path.c_str());
    stage_tracker_collect(true);
    stage_tracker_take_quest_deltas();
    stage_tracker_take_mission_changes();  // the list as loaded is not news
    stage_tracker_take_dialogs();
    log::info("StageReplay: recording the game's own stage branches, dialog answers, quest and mission changes for {} s "
              "in the world", stay_s);
    const uint64_t start = GetTickCount64();
    while (GetTickCount64() - start < stay_s * 1000ull) {
        const bool go_on = wait(stop, 1000);
        append(path, stage_tracker_take_game_branches(), stage_tracker_take_quest_deltas(),
               stage_tracker_take_mission_changes(), stage_tracker_take_dialogs(), start);
        if (!go_on) return;
    }
    stage_tracker_collect(false);
    const auto r = load(path);
    log::info("StageReplay: RESULT: {} recorded {} branches, {} dialog messages, {} quest and {} mission changes",
              r.branches.empty() && r.dialogs.empty() ? "SKIP" : "PASS", r.branches.size(), r.dialogs.size(),
              r.quests.size(), r.missions.size());
}

// Every recorded branch goes through the same queue the network sync uses; the
// recorded quest changes are the oracle: they must come back here too.
constexpr uint16_t kOpQuestDialogEndReq = 2342;

void replay_session(void* stop, const std::wstring& path, uint32_t rng_pin) {
    const auto rec = load(path);
    if (rec.branches.empty() && rec.dialogs.empty()) {
        log::warn("StageReplay: RESULT: SKIP nothing recorded");
        return;
    }
    log::info("StageReplay: replay of {} recorded branches and {} dialog messages; expecting {} quest and {} mission "
              "changes",
              rec.branches.size(), rec.dialogs.size(), rec.quests.size(), rec.missions.size());
    StageSyncQueue queue(StageSyncQueue::Settings{kReplayGiveUpMs, 5000, 1024});
    const uint64_t t0 = GetTickCount64();
    for (const auto& b : rec.branches) queue.push(b, static_cast<uint32_t>(t0));
    stage_tracker_take_quest_deltas();
    stage_tracker_take_mission_changes();
    std::map<uint32_t, uint8_t> seen;
    std::map<uint32_t, std::vector<uint8_t>> seen_missions;
    size_t next_dialog = 0;
    uint64_t dialog_due = 0, dialogs_done_ms = 0;
    bool dialog_baseline = false;
    uint64_t last_progress = t0;
    uint32_t last_applied = 0;
    auto matched = [&] {
        size_t n = 0;
        for (const auto& [k, st] : rec.quests) n += seen.contains(k) && seen.at(k) == st;
        for (const auto& [k, row] : rec.missions) n += seen_missions.contains(k) && seen_missions.at(k) == row;
        return n;
    };
    const size_t expected = rec.quests.size() + rec.missions.size();
    while (GetTickCount64() - t0 < kReplayGiveUpMs) {
        if (!wait(stop, 200)) return;
        if (!in_world()) continue;
        const uint32_t now = static_cast<uint32_t>(GetTickCount64());
        for (const auto& a : queue.tick(stage_tracker_local(), now)) {
            const bool ok = stage_tracker_send_branch(a.instance, a.branch, a.flags);
            log::info("StageReplay: branch {} of stage 0x{:X} -> instance 0x{:X}{}", a.branch, a.key, a.instance,
                      ok ? "" : " (NOT sent)");
        }
        // The recorded dialog: kDialogAfterMs in the world, then in order with the recorded gaps (at most 3 s).
        if (next_dialog < rec.dialogs.size()) {
            const uint64_t t = GetTickCount64();
            if (!dialog_due) dialog_due = t + kDialogAfterMs;
            if (t >= dialog_due) {
                if (!dialog_baseline) {
                    // Anything that changed before our first message is not ours.
                    for (const auto& m : stage_tracker_take_mission_changes()) {
                        log::info("StageReplay: mission 0x{:X} changed before the dialog (ignored)", m.key);
                    }
                    dialog_baseline = true;
                }
                auto d = rec.dialogs[next_dialog];
                // The NPC's runtime id in THIS session (by static key), or wait for it to load.
                uint32_t id = 0;
                if (d.payload.size() >= 4) std::memcpy(&id, d.payload.data(), 4);
                if (const auto it = rec.npcs.find(id); it != rec.npcs.end()) {
                    const uint32_t now_id = stage_tracker_actor_runtime(it->second);
                    if (!now_id) {
                        if (t - dialog_due > kNpcWaitMs) {
                            log::error("StageReplay: NPC with static key 0x{:X} never loaded here; dialog not sent",
                                       it->second);
                            next_dialog = rec.dialogs.size();
                            dialogs_done_ms = t;
                        }
                        continue;
                    }
                    if (now_id != id) {
                        std::memcpy(d.payload.data(), &now_id, 4);
                        log::info("StageReplay: NPC 0x{:X} (static 0x{:X}) is 0x{:X} in this session", id, it->second,
                                  now_id);
                    }
                }
                if (rng_pin && d.op == kOpQuestDialogEndReq) {
                    quest_rng_pin_next(rng_pin);
                    log::info("StageReplay: the server rand state for this answer is pinned to 0x{:08X}", rng_pin);
                }
                const bool ok = stage_tracker_send_dialog(d.op, d.payload);
                log::info("StageReplay: dialog message {} ({} bytes) sent{}", d.op, d.payload.size(),
                          ok ? "" : " - NOT sent (injection refused)");
                if (++next_dialog < rec.dialogs.size()) {
                    const uint64_t gap = rec.dialogs[next_dialog].ms - d.ms;
                    dialog_due = t + (gap < kDialogGapMaxMs ? gap : kDialogGapMaxMs);
                } else {
                    dialogs_done_ms = t;
                }
            }
        }
        for (const auto& m : stage_tracker_take_mission_changes()) {
            if (!dialog_baseline && !rec.dialogs.empty()) continue;
            std::vector<uint8_t> row(m.row.begin(), m.row.end());
            std::string diff;
            if (const auto it = rec.missions.find(m.key); it != rec.missions.end()) {
                // The accept time (game clock) differs per session: not compared, taken from the recording.
                const size_t clock = row.size() > 4 && row[4] == 5 ? 14 : 6;
                if (row.size() >= clock + 4 && it->second.size() >= clock + 4) {
                    std::memcpy(row.data() + clock, it->second.data() + clock, 4);
                }
                for (size_t i = 0; i < row.size() && i < it->second.size(); ++i) {
                    if (row[i] != it->second[i]) diff += std::format(" +{}:{:02X}(recorded {:02X})", i, row[i], it->second[i]);
                }
                diff = diff.empty() ? " (recorded, same row)" : " (recorded slot, row DIFFERS:" + diff + ")";
            }
            log::info("StageReplay: mission 0x{:X} {} kind {} state {}{}", m.key, m.added ? "added" : "changed",
                      m.kind, m.state, diff);
            seen_missions[m.key] = std::move(row);
        }
        for (const auto& q : stage_tracker_take_quest_deltas()) {
            seen[q.key] = q.state;
            log::info("StageReplay: quest 0x{:X} now state {}{}", q.key, q.state,
                      rec.quests.contains(q.key) ? " (recorded)" : "");
        }
        if (queue.counters().applied != last_applied) {
            last_applied = queue.counters().applied;
            last_progress = GetTickCount64();
        }
        // Done: every recorded quest change came back, or nothing moved for 90 s after the last apply.
        if (expected && matched() == expected && next_dialog == rec.dialogs.size() && queue.pending() == 0) break;
        if (last_applied && GetTickCount64() - last_progress > 90000 && next_dialog == rec.dialogs.size()) break;
        if (dialogs_done_ms && rec.branches.empty() && GetTickCount64() - dialogs_done_ms > 60000) break;
    }
    const auto& c = queue.counters();
    const size_t m = matched();
    log::info("StageReplay: branches applied {} | reached alone {} | expired {} | still pending {}", c.applied,
              c.reached_alone, c.expired, queue.pending());
    log::info("StageReplay: dialog messages sent {} of {}", next_dialog, rec.dialogs.size());
    if (!expected) {
        log::info("StageReplay: RESULT: {} {} branches applied (no quest or mission change was recorded to compare)",
                  c.applied ? "PASS" : "FAIL", c.applied);
    } else if (m == expected) {
        log::info("StageReplay: RESULT: PASS all {} recorded quest/mission changes happened here too", m);
    } else {
        log::error("StageReplay: RESULT: FAIL {} of {} recorded quest/mission changes happened here", m, expected);
        for (const auto& [k, row] : rec.missions) {
            if (!seen_missions.contains(k)) log::error("StageReplay:   mission 0x{:X} never came", k);
            else if (seen_missions.at(k) != row) log::error("StageReplay:   mission 0x{:X} came with a different row", k);
        }
    }
    if (!wait(stop, static_cast<uint32_t>(kSurviveMs))) return;
    log::info("StageReplay: RESULT: PASS game still running {} s after the replay", kSurviveMs / 1000);
}

}  // namespace

void stage_replay_loop(void* stop, const std::wstring& dir, bool record, bool replay, uint32_t stay_s,
                       bool quit_after, uint32_t rng_pin) {
    const std::wstring path = dir + L"\\cdmp_stage_record.txt";
    log::warn("StageReplay: {} (test copy only; do not save)", record ? "RECORD" : "REPLAY, it WRITES quest state");
    // Dev diagnostic, read here only: [dev] mission_watch=1|2|3, mission_watch_sig_off, mission_watch_tmpl_off,
    // mission_watch_force_template (decimal).
    const std::wstring ini = dir + L"\\cdmp.ini";
    MissionWatchSettings mw;
    mw.mode = static_cast<int>(GetPrivateProfileIntW(L"dev", L"mission_watch", 0, ini.c_str()));
    mw.sig_off = static_cast<int>(GetPrivateProfileIntW(L"dev", L"mission_watch_sig_off", 0, ini.c_str()));
    mw.tmpl_off = static_cast<int>(GetPrivateProfileIntW(L"dev", L"mission_watch_tmpl_off", 0, ini.c_str()));
    mw.force_template = GetPrivateProfileIntW(L"dev", L"mission_watch_force_template", 0, ini.c_str());
    // mission_watch_force_more = the request's other slots as negative decimals, e.g. "-2057,-2058".
    {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(L"dev", L"mission_watch_force_more", L"", buf, 128, ini.c_str());
        int n = 0;
        for (wchar_t* p = buf; *p && n < 4;) {
            wchar_t* end = nullptr;
            const long v = std::wcstol(p, &end, 10);
            if (end == p) break;
            mw.force_more[n++] = static_cast<uint32_t>(v);
            p = *end == L',' ? end + 1 : end;
        }
    }
    if (replay) mission_watch_configure(mw);
    const bool rng_hooked = quest_rng_install(replay && rng_pin != 0);
    if (rng_pin && !rng_hooked) log::error("StageReplay: rand pin 0x{:08X} requested but the hook is not in", rng_pin);
    while (!in_world()) {
        if (!wait(stop, 500)) return;
    }
    if (record) record_session(stop, path, stay_s);
    else if (replay) replay_session(stop, path, rng_hooked ? rng_pin : 0);
    if (quit_after && wait(stop, 3000)) test_quit_game(20000);
}

}  // namespace cdmp::net
