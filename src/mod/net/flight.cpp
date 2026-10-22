#include "mod/net/flight.h"

#include <windows.h>

#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "cofr.h"
#include "common/story_digest.h"
#include "mod/log.h"
#include "mod/net/stage_tracker.h"

namespace cdmp::net {
namespace {

constexpr uint32_t kDigestEveryMs = 5000;
// The game can end without running our shutdown code (it was closed hard after ignoring
// WM_CLOSE in a test, 2026-10-22): keep a rolling file, at most this old.
constexpr uint32_t kRollingEveryMs = 30000;
uint32_t g_last_rolling_ms = 0;
std::wstring g_rolling;  // <dir>\flight\<label>-latest.cofr

std::unique_ptr<cofr::Recorder> g_rec;
std::mutex g_mutex;
StoryDigest g_shared;                             // what the host shared (common/story_digest.h)
uint32_t g_last_digest_ms = 0;
std::vector<std::pair<uint64_t, uint32_t>> g_detail[2];
std::vector<std::pair<std::pair<uint64_t, uint32_t>, std::string>> g_text;
uint32_t g_epoch = 0;
uint64_t g_seq = 0;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Runs at dump time on every recorded datagram: its type, and for a story entry its number.
bool decode(const uint8_t* data, size_t size, cofr::NetMeta& m) {
    const auto p = proto::decode({data, size});
    if (!p) return false;
    m.msgType = static_cast<uint8_t>(p->body.index());
    if (const auto* d = std::get_if<proto::RelData>(&p->body); d && d->kind == proto::RelKind::Story && d->index == 0) {
        if (const auto e = proto::decode_story(d->chunk)) {
            m.epoch = e->log_id;
            m.seqLo = m.seqHi = e->seq;
        }
    }
    return true;
}

void note_shared(const proto::StoryEntry& e) {
    std::lock_guard lock(g_mutex);
    g_shared.note(e);
}

uint64_t entry_hash(const proto::StoryEntry& e) {
    const uint8_t k = static_cast<uint8_t>(e.kind);
    return cofr::hash64(e.bytes.data(), e.bytes.size(), cofr::hash64(&k, 1));
}

const char* kSchemaJson = R"json({"game":"crimson-desert (cdmp)",
"digest_categories":{"0":"shared stage branches","1":"shared mission rows"},
"message_types":{"0":"Hello","1":"Welcome","2":"Bye","3":"Ping","4":"Pong","5":"PlayerState","6":"GameEvent","7":"RelData","8":"RelAck"}})json";

}  // namespace

void flight_configure(bool enabled, const std::wstring& dir, bool host, const std::string& label) {
    if (!enabled) return;
    const std::wstring folder = dir + L"\\flight";
    CreateDirectoryW(folder.c_str(), nullptr);
    cofr::Config c;
    c.slot = host ? 0 : 1;
    c.role = host ? cofr::kRoleHost : cofr::kRoleClient;
    c.protoVersion = proto::kVersion;
    std::string safe = label;  // goes into a file name
    for (char& ch : safe) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) ch = '_';
    }
    c.label = (host ? "host-" : "guest-") + safe.substr(0, 32);
    c.dir = utf8(folder);
    c.schemaJson = kSchemaJson;
    c.decoder = decode;
    g_rec = std::make_unique<cofr::Recorder>(c);
    g_rolling = folder + L"\\" + std::wstring(c.label.begin(), c.label.end()) + L"-latest.cofr";
    g_rec->mapPeer(0, host ? 1 : 0);  // transport peer 0 = the other PC
    g_rec->meta("game.build", "1.0.0.2976");
    log::info("Flight recorder: ON (last 16 MB; written to {} on exit, Ctrl+F12 and when a shared accept fails)",
              c.dir);
}

bool flight_enabled() { return g_rec != nullptr; }

void flight_net_out(const uint8_t* data, size_t size) {
    if (g_rec) g_rec->netOut(0, data, size);
}
void flight_net_in(const uint8_t* data, size_t size) {
    if (g_rec) g_rec->netIn(0, data, size);
}

void flight_story_captured(const proto::StoryEntry& e) {
    if (!g_rec) return;
    note_shared(e);
    cofr::CapturedEvent c;
    c.seq = e.seq;
    c.epoch = e.log_id;
    c.effectCount = 1;
    c.hash = entry_hash(e);
    c.cause = static_cast<uint8_t>(e.kind);
    g_rec->captured(c);
}

void flight_story_applied(const proto::StoryEntry& e) {
    if (!g_rec) return;
    note_shared(e);
    cofr::AppliedEvent a;
    a.seq = e.seq;
    a.epoch = e.log_id;
    a.origin = 0;
    a.effectTag = static_cast<uint8_t>(e.kind);
    a.hash = entry_hash(e);
    g_rec->applied(a);
}

namespace {
void write_detail();
void write_rolling(uint32_t now_ms) {
    if (now_ms - g_last_rolling_ms < kRollingEveryMs) return;
    g_last_rolling_ms = now_ms;
    write_detail();
    const std::wstring tmp = g_rolling + L".tmp";
    if (g_rec->dump(utf8(tmp), cofr::kDumpManual)) MoveFileExW(tmp.c_str(), g_rolling.c_str(), MOVEFILE_REPLACE_EXISTING);
}
}  // namespace

void flight_tick(uint32_t now_ms, uint32_t log_id, uint32_t last_seq) {
    if (!g_rec) return;
    write_rolling(now_ms);
    if (now_ms - g_last_digest_ms < kDigestEveryMs) return;
    g_last_digest_ms = now_ms;
    StoryDigest shared;
    {
        std::lock_guard lock(g_mutex);
        shared = g_shared;
    }
    if (shared.empty()) return;
    const auto local = stage_tracker_local();
    const std::vector<StoryDigest::Entry> cats[2] = {
        shared.stages([&](uint32_t key) -> std::optional<std::pair<uint32_t, uint32_t>> {
            for (const auto& l : local) {
                if (l.key == key) return std::make_pair(l.state, l.branch);
            }
            return std::nullopt;
        }),
        shared.missions([](uint32_t key) { return stage_tracker_mission_kind(key); }),
    };
    std::vector<std::pair<std::pair<uint64_t, uint32_t>, std::string>> text;
    for (uint8_t c = 0; c < 2; ++c) {
        cofr::BucketedState st;
        for (const auto& e : cats[c]) {
            st.set(e.key, e.value_hash);
            text.push_back({{e.key, e.value_hash}, e.text});
        }
        g_rec->digest(st.digest(c, log_id, last_seq), cofr::kFlagBucketed);
        std::lock_guard lock(g_mutex);
        g_detail[c] = st.entries();
    }
    std::lock_guard lock(g_mutex);
    g_text = std::move(text);
    g_epoch = log_id;
    g_seq = last_seq;
}

void flight_marker(const std::string& text) {
    if (g_rec) g_rec->marker(cofr::kMarkNote, text);
}

namespace {
void write_detail() {
    std::lock_guard lock(g_mutex);
    for (uint8_t c = 0; c < 2; ++c) {
        if (!g_detail[c].empty()) g_rec->digestDetail(c, g_epoch, g_seq, g_detail[c]);
    }
    for (const auto& [kv, t] : g_text) g_rec->entryText(kv.first, kv.second, t);
}
}  // namespace

void flight_dump(const char* why) {
    if (!g_rec) return;
    write_detail();
    g_rec->marker(cofr::kMarkNote, why);
    const bool shutdown = std::string(why) == "shutdown";
    const std::string path = g_rec->dumpToDir(shutdown ? cofr::kDumpShutdown : cofr::kDumpManual);
    log::info("Flight recorder: {} -> {}", why, path.empty() ? std::string("NOT written") : path);
}

void flight_desync(const std::string& what) {
    if (!g_rec) return;
    write_detail();
    const std::string path = g_rec->onDesync(what);
    log::warn("Flight recorder: {} -> {}", what, path.empty() ? std::string("(rate limited, not written)") : path);
}

}  // namespace cdmp::net
