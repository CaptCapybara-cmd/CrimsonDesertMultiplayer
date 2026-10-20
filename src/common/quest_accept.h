#pragma once
// Sharing quest ACCEPTS from the host to the guest (Crimson Desert's "requests"),
// no game or network in here; the tests drive it. Policy (owner, 2026-10-20): the
// guest gets every quest the host accepts from an NPC; each player then does and
// hands in their own; hand-ins and rewards are never shared. The meeting scene that
// makes the NPC known travels separately, as stage branches (stage_sync.h), before
// the accept. Coop-anything games/crimson-desert/NOTES.md 2026-10-20.
//
// Host: the game answers a quest dialog (QuestDialogEndReq). Within window_ms the
// server either removes items (a hand-in: dropped) or changes mission rows without
// removing anything (an accept: shared, with the rows it changed).
//
// Guest: per accept, oldest first, wait until
//   - the NPC (by static key) is loaded here and we know our own player id, and
//   - every step's mission row has the host's "before" kind here (0 = no row);
// then open the conversation, answer, close it (the game's own order), and check
// that every row reaches the host's "after" kind. Already there: done without
// sending. Never ready within expire_ms: given up. Each accept is sent at most once.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "common/protocol.h"

namespace cdmp {

class AcceptDetector {
public:
    struct Settings {
        uint32_t window_ms = 400;
    };
    AcceptDetector() = default;
    explicit AcceptDetector(Settings s) : s_(s) {}

    // The game sent a quest dialog answer; `npc_key` = its NPC's static key (0 = unknown).
    void on_answer(const std::vector<uint8_t>& payload, uint32_t npc_key, uint32_t now_ms) {
        p_ = Pending{payload, npc_key, now_ms, false, {}};
    }
    void on_item_removed(uint32_t now_ms) {
        if (p_ && now_ms - p_->ms <= s_.window_ms) p_->removed = true;
    }
    // A mission row changed (`before` 0 = new row).
    void on_mission(uint32_t key, uint8_t before, uint8_t after, uint32_t now_ms) {
        if (!p_ || now_ms - p_->ms > s_.window_ms || before == after) return;
        for (auto& st : p_->steps) {
            if (st.key == key) {
                st.after = after;
                return;
            }
        }
        if (p_->steps.size() < proto::kMaxAcceptSteps) p_->steps.push_back({key, before, after});
    }
    // An accept, once its window is over.
    std::optional<proto::QuestAcceptMsg> tick(uint32_t now_ms) {
        if (!p_ || now_ms - p_->ms <= s_.window_ms) return std::nullopt;
        std::optional<proto::QuestAcceptMsg> out;
        if (p_->removed) {
            ++handins_;
        } else if (!p_->steps.empty() && p_->npc_key && p_->payload.size() >= 4 &&
                   p_->payload.size() <= proto::kMaxAcceptAnswer) {
            out = proto::QuestAcceptMsg{p_->npc_key, p_->payload, p_->steps};
            ++accepts_;
        } else {
            ++other_;
        }
        p_.reset();
        return out;
    }
    uint32_t accepts() const { return accepts_; }
    uint32_t handins() const { return handins_; }
    uint32_t other() const { return other_; }

private:
    struct Pending {
        std::vector<uint8_t> payload;
        uint32_t npc_key;
        uint32_t ms;
        bool removed;
        std::vector<proto::QuestAcceptMsg::Step> steps;
    };
    Settings s_;
    std::optional<Pending> p_;
    uint32_t accepts_ = 0, handins_ = 0, other_ = 0;
};

// What the guest sends to its own server: (opcode, payload).
struct DialogSend {
    uint16_t op = 0;
    std::vector<uint8_t> payload;
};

class GuestAcceptQueue {
public:
    static constexpr uint16_t kOpQuestDialogEndReq = 2342;
    static constexpr uint16_t kOpSetQuestDialogObstacleReq = 2621;
    struct Settings {
        uint32_t expire_ms = 10 * 60 * 1000;  // the guest may be far from the NPC for a while
        uint32_t verify_ms = 10000;
        uint32_t gap_ms = 200;                // between the conversation's messages
        size_t max_pending = 32;
    };
    struct Counters {
        uint32_t received = 0, refused = 0, sent = 0, accepted = 0, already = 0, failed = 0, expired = 0;
    };
    // The guest's world, asked each tick.
    struct World {
        std::function<uint32_t(uint32_t npc_key)> npc_runtime;      // 0 = not loaded here
        std::function<int(uint32_t mission_key)> mission_kind;     // -1 = no such row
        uint32_t own_player = 0;                                   // 0 = not known yet
    };

    GuestAcceptQueue() = default;
    explicit GuestAcceptQueue(Settings s) : s_(s) {}

    bool push(const proto::QuestAcceptMsg& m, uint32_t now_ms) {
        if (q_.size() >= s_.max_pending) {
            ++c_.refused;
            return false;
        }
        q_.push_back({m, now_ms, Phase::Waiting, 0, 0});
        ++c_.received;
        return true;
    }

    std::vector<DialogSend> tick(const World& w, uint32_t now_ms) {
        std::vector<DialogSend> out;
        while (!q_.empty()) {
            Item& it = q_.front();
            const auto kind = [&](uint32_t key) { return w.mission_kind ? w.mission_kind(key) : -1; };
            const auto at = [&](bool after) {
                for (const auto& s : it.m.steps) {
                    const int want = after ? s.after : s.before;
                    const int have = kind(s.key);
                    if (!(have == want || (want == 0 && have < 0))) return false;
                }
                return true;
            };
            if (it.phase == Phase::Waiting) {
                if (at(true)) {
                    ++c_.already;
                    pop();
                    continue;
                }
                if (now_ms - it.received_ms > s_.expire_ms) {
                    ++c_.expired;
                    pop();
                    continue;
                }
                const uint32_t npc = w.npc_runtime ? w.npc_runtime(it.m.npc_key) : 0;
                if (!npc || !w.own_player || !at(false)) break;  // later accepts wait for this one
                it.npc = npc;
                it.phase = Phase::Sending;
                it.step = 0;
                it.next_ms = now_ms;
            }
            if (it.phase == Phase::Sending) {
                if (now_ms < it.next_ms) break;
                out.push_back(message(it, w.own_player));
                it.next_ms = now_ms + s_.gap_ms;
                if (++it.step == kSteps) {
                    ++c_.sent;
                    it.phase = Phase::Verifying;
                    it.sent_ms = now_ms;
                }
                break;
            }
            if (it.phase == Phase::Verifying) {
                if (at(true)) {
                    ++c_.accepted;
                    pop();
                    continue;
                }
                if (now_ms - it.sent_ms > s_.verify_ms) {
                    ++c_.failed;
                    pop();
                    continue;
                }
                break;
            }
        }
        return out;
    }

    size_t pending() const { return q_.size(); }
    const Counters& counters() const { return c_; }
    void clear() { q_.clear(); }

private:
    enum class Phase { Waiting, Sending, Verifying };
    struct Item {
        proto::QuestAcceptMsg m;
        uint32_t received_ms;
        Phase phase;
        uint32_t npc = 0;
        int step = 0;
        uint32_t next_ms = 0;
        uint32_t sent_ms = 0;
    };
    // Open (player, NPC), answer, close (player, NPC): the game's own order.
    static constexpr int kSteps = 5;

    static DialogSend obstacle(uint32_t actor, bool on) {
        DialogSend d{kOpSetQuestDialogObstacleReq, std::vector<uint8_t>(5)};
        std::memcpy(d.payload.data(), &actor, 4);
        d.payload[4] = on ? 1 : 0;
        return d;
    }
    static DialogSend message(const Item& it, uint32_t player) {
        switch (it.step) {
            case 0: return obstacle(player, true);
            case 1: return obstacle(it.npc, true);
            case 2: {
                DialogSend d{kOpQuestDialogEndReq, it.m.answer};
                std::memcpy(d.payload.data(), &it.npc, 4);
                return d;
            }
            case 3: return obstacle(player, false);
            default: return obstacle(it.npc, false);
        }
    }
    void pop() { q_.pop_front(); }

    Settings s_;
    std::deque<Item> q_;
    Counters c_;
};

}  // namespace cdmp
