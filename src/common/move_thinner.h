#pragma once
// Receiver-side thinning of the other player's Moves to about 40 Hz
// (saltos-copia-anfitrion.md P2): the sender's game produces 67-83 Moves a
// second, but the client keeps at most 30 samples of a remote actor (build
// 2976, exe+0x6CB9598). With ~400 ms of future samples queued that overflows;
// the client evicts the oldest sample and, when all are future, stops the
// actor until it catches up: the copy stalls, then jumps. 25-40 Hz is enough,
// the client derives speed from the interpolation.
//
// Rules (sender game clock):
//   - drop a Move less than min_gap_ms after the last one kept, except
//     the first after a silence (gap_ms) and one whose parent changes
//     (mounting, dismounting, boarding);
//   - a dropped Move is held, not lost: if nothing newer arrives within
//     release_ms (wall clock), it goes in after all, so the copy stops where
//     the sender stopped.
// No game or network in here; the tests drive it.

#include <cstdint>

namespace cdmp {

class MoveThinner {
public:
    struct Settings {
        uint64_t min_gap_ms = 22;
        uint64_t gap_ms = 150;
        uint64_t release_ms = 60;
    };
    enum class Verdict { Keep, Hold };

    MoveThinner() = default;
    explicit MoveThinner(Settings s) : s_(s) {}

    // A Move with the sender's game timestamp `ts`, its parent, arriving at wall time `now`.
    Verdict offer(uint64_t ts, uint32_t parent, uint64_t now) {
        const bool first = !have_;
        const bool after_gap = have_ && ts > last_seen_ts_ + s_.gap_ms;
        const bool parent_changed = have_ && parent != last_parent_;
        const bool out_of_order = have_ && ts < last_kept_ts_;
        last_seen_ts_ = have_ ? (ts > last_seen_ts_ ? ts : last_seen_ts_) : ts;
        last_parent_ = parent;
        have_ = true;
        if (first || after_gap || parent_changed || out_of_order || ts >= last_kept_ts_ + s_.min_gap_ms) {
            keep(ts);
            return Verdict::Keep;
        }
        held_ = true;
        held_ts_ = ts;
        held_arrival_ = now;
        ++dropped_;
        return Verdict::Hold;
    }

    // True once the held Move is due (nothing newer for release_ms): the caller
    // sends it now. It then counts as kept.
    bool release_due(uint64_t now) {
        if (!held_ || now < held_arrival_ + s_.release_ms) return false;
        keep(held_ts_);
        --dropped_;
        ++released_;
        return true;
    }

    bool holding() const { return held_; }
    uint64_t kept() const { return kept_; }
    uint64_t dropped() const { return dropped_; }      // held and replaced by a newer Move
    uint64_t released() const { return released_; }    // held, then sent after a silence
    void reset() { *this = MoveThinner(s_); }

private:
    void keep(uint64_t ts) {
        last_kept_ts_ = ts;
        held_ = false;
        ++kept_;
    }

    Settings s_;
    bool have_ = false;
    uint64_t last_seen_ts_ = 0, last_kept_ts_ = 0;
    uint32_t last_parent_ = 0;
    bool held_ = false;
    uint64_t held_ts_ = 0, held_arrival_ = 0;
    uint64_t kept_ = 0, dropped_ = 0, released_ = 0;
};

}  // namespace cdmp
