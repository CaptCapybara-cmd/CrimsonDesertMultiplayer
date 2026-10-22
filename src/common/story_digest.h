#pragma once
// What the flight recorder compares between host and guest (mod/net/flight.h).
// Category 0: for every stage key the host shared, the host's last shared branch (equal on
// both PCs once the guest has the entries; this world's live state is in the text only:
// stage sync shares ambient stages too, which cycle on each PC by themselves and would
// look like divergences, seen in the 2026-10-22 in-game recording). Category 1: for every
// mission row the host's accepts changed, this PC's kind of it (the real check). Keys are static, so both PCs digest the same set once the guest has
// the same entries. Shared by the mod and the fuzz target (tests/fuzz), so both compute it
// the same way. No game in here.

#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/protocol.h"

namespace cdmp {

class StoryDigest {
public:
    struct Entry {
        uint64_t key;
        uint32_t value_hash;
        std::string text;
    };
    // (state, branch) of this world's instance of a stage key, or nullopt when it has none.
    using StageFn = std::function<std::optional<std::pair<uint32_t, uint32_t>>(uint32_t key)>;
    using MissionFn = std::function<int(uint32_t key)>;  // -1 = no such row

    void note(const proto::StoryEntry& e) {
        if (e.kind == proto::RelKind::StageBranch) {
            if (const auto b = proto::decode_stage_branch(e.bytes)) stages_[b->key] = b->branch;
        } else if (e.kind == proto::RelKind::QuestAccept) {
            if (const auto a = proto::decode_quest_accept(e.bytes)) {
                for (const auto& s : a->steps) missions_[s.key] = s.after;
            }
        }
    }
    bool empty() const { return stages_.empty() && missions_.empty(); }

    std::vector<Entry> stages(const StageFn& f) const {
        std::vector<Entry> out;
        for (const auto& [k, last] : stages_) {
            const auto v = f(k);
            const std::string here = v ? std::format("state {} branch {}", v->first, static_cast<int32_t>(v->second))
                                       : std::string("not in this world");
            out.push_back({k, mix(last, 0x53544147u), std::format("stage 0x{:X}: host's last shared branch {} (here: {})", k, last, here)});
        }
        return out;
    }
    std::vector<Entry> missions(const MissionFn& f) const {
        std::vector<Entry> out;
        for (const auto& [k, after] : missions_) {
            const int kind = f(k);
            out.push_back({k, mix(static_cast<uint32_t>(kind), 0x4D495353u),
                           std::format("mission 0x{:X}: kind {} (the host's accept made it {}){}", k, kind, after,
                                       kind == after ? "" : " - not (yet) here")});
        }
        return out;
    }

    // FNV-1a over the two words: equal values hash equal on every PC.
    static uint32_t mix(uint32_t a, uint32_t b) {
        uint32_t h = 2166136261u;
        for (uint32_t w : {a, b}) {
            for (int i = 0; i < 4; ++i) {
                h ^= (w >> (8 * i)) & 0xFF;
                h *= 16777619u;
            }
        }
        return h;
    }

private:
    std::map<uint32_t, uint32_t> stages_;  // key -> the host's last shared branch
    std::map<uint32_t, uint8_t> missions_;  // key -> the host's kind after the accept
};

}  // namespace cdmp
