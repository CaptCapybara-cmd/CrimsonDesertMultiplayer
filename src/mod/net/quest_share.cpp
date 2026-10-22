#include "mod/net/quest_share.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <string>

#include "common/quest_accept.h"
#include "mod/log.h"
#include "mod/net/stage_tracker.h"
#include "mod/net/troc_sniffer.h"
#include "mod/test_input.h"

namespace cdmp::net {
namespace {

constexpr uint64_t kFeedAfterMs = 25000;  // in the world this long (after the stage feed) before the test accept
constexpr uint64_t kFeedGiveUpMs = 6 * 60 * 1000;
constexpr uint64_t kSurviveMs = 30000;

QuestShareSettings g_cfg;
AcceptDetector g_detector;
GuestAcceptQueue g_queue;
bool g_collecting = false;
bool g_was_loading = false;
uint64_t g_in_world_since = 0;
GuestAcceptQueue::Counters g_logged{};

enum class Feed { Waiting, Sent, Accepted, Done };
Feed g_feed = Feed::Waiting;
uint64_t g_feed_ms = 0;

uint32_t now32() { return static_cast<uint32_t>(GetTickCount64()); }

std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> out;
    for (; s[0] && s[1]; s += 2) {
        unsigned v = 0;
        if (std::sscanf(s, "%2x", &v) != 1) return {};
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

// The recorded accept: the QuestDialogEndReq line, its NPC, and the rows it changed.
std::optional<proto::QuestAcceptMsg> load_feed(const std::wstring& path) {
    proto::QuestAcceptMsg m;
    std::map<uint32_t, uint32_t> npcs;
    std::ifstream in(path);
    std::string line;
    char buf[512];
    while (std::getline(in, line)) {
        unsigned a = 0, b = 0, c = 0;
        unsigned long long ms = 0;
        if (std::sscanf(line.c_str(), "D %u %llu %511s", &a, &ms, buf) == 3 && a == 2342 && m.answer.empty()) {
            m.answer = unhex(buf);
        } else if (std::sscanf(line.c_str(), "N %x %x", &a, &b) == 2) {
            npcs[a] = b;
        } else if (std::sscanf(line.c_str(), "S %x %u %u", &a, &b, &c) == 3) {
            m.steps.push_back({a, static_cast<uint8_t>(b), static_cast<uint8_t>(c)});
        }
    }
    if (m.answer.size() < 4 || m.steps.empty()) return std::nullopt;
    uint32_t id = 0;
    std::memcpy(&id, m.answer.data(), 4);
    const auto it = npcs.find(id);
    if (it == npcs.end()) return std::nullopt;
    m.npc_key = it->second;
    return m;
}

void quit_later() {
    CloseHandle(CreateThread(
        nullptr, 0, [](LPVOID) -> DWORD { test_quit_game(20000); return 0; }, nullptr, 0, nullptr));
}

void feed_test(bool in_world, std::vector<proto::QuestAcceptMsg>& out) {
    if (g_cfg.feed_path.empty() || g_feed == Feed::Done) return;
    const uint64_t now = GetTickCount64();
    const auto& c = g_queue.counters();
    switch (g_feed) {
        case Feed::Waiting:
            if (in_world && g_in_world_since && now - g_in_world_since >= kFeedAfterMs) {
                const auto m = load_feed(g_cfg.feed_path);
                if (!m) {
                    log::warn("QuestShare: RESULT: SKIP no recorded accept to feed (D 2342, N and S lines)");
                    g_feed = Feed::Done;
                    if (g_cfg.quit_after) quit_later();
                    return;
                }
                log::info("QuestShare: test feed: sending the recorded accept (NPC 0x{:X}, {} rows) as the host's",
                          m->npc_key, m->steps.size());
                out.push_back(*m);
                g_feed = Feed::Sent;
                g_feed_ms = now;
            }
            break;
        case Feed::Sent:
            if (c.accepted > 0) {
                log::info("QuestShare: RESULT: PASS the accept from the network was applied here ({})",
                          quest_share_describe());
                g_feed = Feed::Accepted;
                g_feed_ms = now;
            } else if (c.failed || c.expired || now - g_feed_ms > kFeedGiveUpMs) {
                log::error("QuestShare: RESULT: FAIL ({})", quest_share_describe());
                g_feed = Feed::Done;
                if (g_cfg.quit_after) quit_later();
            }
            break;
        case Feed::Accepted:
            if (now - g_feed_ms >= kSurviveMs) {
                log::info("QuestShare: RESULT: PASS game still running {} s after the accept", kSurviveMs / 1000);
                g_feed = Feed::Done;
                if (g_cfg.quit_after) quit_later();
            }
            break;
        case Feed::Done:
            break;
    }
}

}  // namespace

void quest_share_configure(const QuestShareSettings& s) {
    g_cfg = s;
    if (s.enabled) {
        log::warn("Quest share: ON (experimental: the host's quest accepts are replayed on the guest){}",
                  s.feed_path.empty() ? "" : " | test feed from cdmp_stage_record.txt");
    }
}

std::vector<proto::QuestAcceptMsg> quest_share_tick(bool linked, bool host, bool echo, bool in_world) {
    std::vector<proto::QuestAcceptMsg> out;
    if (!g_cfg.enabled) return out;
    const bool loading = sniffer_world_loading();
    // A loading screen keeps the queue: it names NPCs by static key and looks the runtime id up
    // each time (clearing it lost the host's accepts: found by the fuzzer, tests/fuzz).
    g_was_loading = loading;
    const uint64_t now = GetTickCount64();
    if (in_world && !loading) {
        if (!g_in_world_since) g_in_world_since = now;
    } else {
        g_in_world_since = 0;
    }
    // Host: our game's own answers, classified once their window is over.
    const bool send = linked && (host || echo);
    if (send != g_collecting) {
        stage_tracker_quest_events(send);
        g_collecting = send;
    }
    if (send) {
        for (const auto& e : stage_tracker_take_quest_events()) {
            const uint32_t t = static_cast<uint32_t>(e.ms);
            switch (e.type) {
                case QuestEvent::Type::Answer: g_detector.on_answer(e.payload, e.npc_key, t); break;
                case QuestEvent::Type::ItemRemoved: g_detector.on_item_removed(t); break;
                case QuestEvent::Type::Mission:
                    g_detector.on_mission(e.mission.key, e.mission.prev_kind, e.mission.kind, t);
                    break;
            }
        }
        while (auto a = g_detector.tick(now32())) {
            log::info("Quest share: we accepted a quest from NPC 0x{:X} ({} mission rows); sending it", a->npc_key,
                      a->steps.size());
            out.push_back(std::move(*a));
        }
        feed_test(in_world && !loading, out);
    }
    // Guest: apply the host's accepts to our own server.
    if (linked && (!host || echo) && in_world && !loading) {
        GuestAcceptQueue::World w;
        w.npc_runtime = [](uint32_t key) { return stage_tracker_actor_runtime(key); };
        w.mission_kind = [](uint32_t key) { return stage_tracker_mission_kind(key); };
        w.own_player = stage_tracker_own_player();
        for (const auto& d : g_queue.tick(w, now32())) {
            const bool ok = stage_tracker_send_dialog(d.op, d.payload);
            log::info("Quest share: the host's accept: message {} ({} bytes) to our server{}", d.op, d.payload.size(),
                      ok ? "" : " - NOT sent (injection refused)");
        }
        const auto& c = g_queue.counters();
        if (std::memcmp(&c, &g_logged, sizeof c) != 0) {
            log::info("Quest share: {}", quest_share_describe());
            g_logged = c;
        }
    }
    return out;
}

void quest_share_on_message(std::span<const uint8_t> bytes) {
    if (!g_cfg.enabled) return;
    const auto m = proto::decode_quest_accept(bytes);
    if (!m) {
        log::warn("Quest share: malformed QuestAccept message ({} bytes) ignored", bytes.size());
        return;
    }
    log::info("Quest share: the host accepted a quest from NPC 0x{:X} ({} mission rows); queued", m->npc_key,
              m->steps.size());
    if (!g_queue.push(*m, now32())) log::warn("Quest share: queue full; accept dropped");
}

std::string quest_share_describe() {
    const auto& c = g_queue.counters();
    return std::format("received {} | sent to our server {} | accepted {} | already here {} | failed {} | "
                       "gave up {} | refused {} | pending {} | host side: accepts {} hand-ins {} other {}",
                       c.received, c.sent, c.accepted, c.already, c.failed, c.expired, c.refused, g_queue.pending(),
                       g_detector.accepts(), g_detector.handins(), g_detector.other());
}

}  // namespace cdmp::net
