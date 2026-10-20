#pragma once
// Quest stage sync, the GUEST's queue (no game or network in it; the tests
// drive it). The host sends each stage branch its game chooses, named by the
// stage's static sequencer key (proto::StageBranchMsg); the guest finds its
// own instance of that stage and sends the same StageBranchStateReq to its own
// server (stage_sync.cpp). Coop-anything games/crimson-desert/NOTES.md, 16:46.
//
// Per key, the oldest branch first:
//   - a local instance of the key is waiting (state 0, no branch, and our own
//     game is not just choosing a branch for it): send it
//     (again after retry_ms if no answer came);
//   - a local instance already has that branch: done (applied if we sent it,
//     otherwise the guest's game got there by itself);
//   - the host's previous branch of this key is done here, and an instance sits
//     on it (not busy): send the next one to that instance. A scene runs through
//     several branches (the butcher shop: 6, 0, 3, 1) and the game sends the later
//     ones while the stage is not waiting (NOTES.md 2026-10-20 15:20);
//   - otherwise (no such stage here yet, or it went another way): hold, and
//     drop it after expire_ms.
// Only branches the host's game really chose are ever sent: a made-up branch
// number crashes the game.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <deque>
#include <iterator>
#include <map>
#include <vector>

#include "common/protocol.h"

namespace cdmp {

// A branch line of cdmp_stage_record.txt: exactly "KKKKKKKK branch flags" (8 hex digits).
// Other lines start with a letter ("D 2621 ...", "N ...", "M ...", "Q ...", "S ...") and a
// loose "%x" reads "D" as key 0xD: a made-up branch, which can crash the game.
inline std::optional<proto::StageBranchMsg> parse_branch_line(std::string_view line) {
    if (line.size() < 12 || line[8] != ' ') return std::nullopt;
    for (size_t i = 0; i < 8; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(line[i]))) return std::nullopt;
    }
    char buf[64];
    const size_t n = line.size() < sizeof buf - 1 ? line.size() : sizeof buf - 1;
    line.copy(buf, n);
    buf[n] = 0;
    unsigned key = 0, branch = 0, flags = 0;
    char extra = 0;
    if (std::sscanf(buf, "%8x %u %u %c", &key, &branch, &flags, &extra) != 3) return std::nullopt;
    if (!key || branch == 0xFFFFFFFF || flags > 0xFFFF) return std::nullopt;
    return proto::StageBranchMsg{key, branch, static_cast<uint16_t>(flags)};
}

struct LocalStage {
    uint64_t instance = 0;
    uint32_t key = 0;     // sequencer key, 0 = not known yet
    uint32_t state = 0;   // 0 = waiting
    uint32_t branch = 0xFFFFFFFF;
    bool busy = false;    // our game asked for a branch and the answer has not been seen yet
};

struct StageAction {
    uint64_t instance = 0;
    uint32_t key = 0;
    uint32_t branch = 0;
    uint16_t flags = 0;
};

class StageSyncQueue {
public:
    struct Settings {
        uint32_t expire_ms = 120000;
        uint32_t retry_ms = 5000;
        size_t max_pending = 256;
    };
    struct Counters {
        uint32_t received = 0, refused = 0, sent = 0, applied = 0, reached_alone = 0, expired = 0;
    };

    StageSyncQueue() = default;
    explicit StageSyncQueue(Settings s) : s_(s) {}

    // False (and counted) when too much is pending.
    bool push(const proto::StageBranchMsg& m, uint32_t now_ms) {
        if (size_ >= s_.max_pending) {
            ++c_.refused;
            return false;
        }
        by_key_[m.key].push_back({m.branch, m.flags, now_ms, 0});
        ++size_;
        ++c_.received;
        return true;
    }

    // What to send now, given the guest's own stages.
    std::vector<StageAction> tick(const std::vector<LocalStage>& local, uint32_t now_ms) {
        std::vector<StageAction> out;
        for (auto it = by_key_.begin(); it != by_key_.end();) {
            auto& q = it->second;
            while (!q.empty()) {
                Entry& e = q.front();
                const LocalStage* waiting = nullptr;
                const LocalStage* chained = nullptr;
                const auto prev = last_.find(it->first);
                bool reached = false;
                for (const auto& l : local) {
                    if (l.key != it->first) continue;
                    // On that branch; after a scene's last branch the state can be 0 again (the
                    // butcher shop's branch 1), which counts when we sent it.
                    if (l.branch == e.branch && (l.state != 0 || e.sent_ms)) reached = true;
                    if (l.state == 0 && l.branch == 0xFFFFFFFF && !l.busy && !waiting) waiting = &l;
                    if (prev != last_.end() && l.state != 0 && l.branch == prev->second && !l.busy && !chained) {
                        chained = &l;
                    }
                }
                if (reached) {
                    ++(e.sent_ms ? c_.applied : c_.reached_alone);
                    last_[it->first] = e.branch;
                    q.pop_front();
                    --size_;
                    continue;
                }
                if (now_ms - e.received_ms > s_.expire_ms) {
                    ++c_.expired;
                    last_.erase(it->first);
                    q.pop_front();
                    --size_;
                    continue;
                }
                if (!waiting) waiting = chained;
                if (waiting && (!e.sent_ms || now_ms - e.sent_ms >= s_.retry_ms)) {
                    e.sent_ms = now_ms ? now_ms : 1;
                    out.push_back({waiting->instance, it->first, e.branch, e.flags});
                    ++c_.sent;
                }
                break;  // later branches of this stage wait for this one
            }
            it = q.empty() ? by_key_.erase(it) : std::next(it);
        }
        return out;
    }

    size_t pending() const { return size_; }
    const Counters& counters() const { return c_; }
    void clear() {
        by_key_.clear();
        last_.clear();
        size_ = 0;
    }

private:
    struct Entry {
        uint32_t branch;
        uint16_t flags;
        uint32_t received_ms;
        uint32_t sent_ms;
    };
    Settings s_;
    std::map<uint32_t, std::deque<Entry>> by_key_;
    std::map<uint32_t, uint32_t> last_;  // key -> the host's last branch done here
    size_t size_ = 0;
    Counters c_;
};

}  // namespace cdmp
