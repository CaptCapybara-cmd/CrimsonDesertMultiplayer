#pragma once
// The mission list as the server keeps telling it (UpdateMissionListAck 3009,
// build 2976): `u8 scope | u32 player | u8 a | u8 b`, then 55-byte rows
// `u32 key | u8 kind | u8 state | ...`. The server re-sends the whole list
// (a = 0) about every 10 s, so "a = 0" is not news by itself: a row is news
// when it is new or differs from the last copy of it. "Request" quests show up
// only here (accepting one added keys 0xFFFFF7F8/7/6; NOTES.md 2026-10-20).
// No game in here; the tests drive it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <vector>

namespace cdmp {

// Rows hold the game-clock time of the accept (+14 in a kind-5 row, else +6); it
// differs between sessions (it was first misread as a "request variant", NOTES.md
// 2026-10-20 15:15).
struct MissionChange {
    uint32_t key = 0;
    uint8_t kind = 0;
    uint8_t state = 0;
    bool added = false;  // first time this key is seen
    uint8_t prev_kind = 0;  // the kind before this change (0 when added)
    std::array<uint8_t, 55> row{};
};

class MissionRows {
public:
    static constexpr size_t kHeader = 7;
    static constexpr size_t kRow = 55;
    static constexpr size_t kMaxRows = 8192;

    // `payload` = the message after its 5-byte header. Returns the rows that are new or changed.
    std::vector<MissionChange> observe(std::span<const uint8_t> payload) {
        std::vector<MissionChange> out;
        if (payload.size() < kHeader || (payload.size() - kHeader) % kRow != 0) return out;
        for (size_t off = kHeader; off + kRow <= payload.size(); off += kRow) {
            Row row;
            std::memcpy(row.data(), payload.data() + off, kRow);
            uint32_t key;
            std::memcpy(&key, row.data(), 4);
            auto it = rows_.find(key);
            const bool added = it == rows_.end();
            if (!added && it->second == row) continue;
            if (added && rows_.size() >= kMaxRows) continue;
            const uint8_t prev = added ? 0 : it->second[4];
            rows_[key] = row;
            out.push_back({key, row[4], row[5], added, prev, row});
        }
        return out;
    }

    size_t size() const { return rows_.size(); }
    // A row's kind, -1 when there is no such row.
    int kind(uint32_t key) const {
        const auto it = rows_.find(key);
        return it == rows_.end() ? -1 : it->second[4];
    }
    void clear() { rows_.clear(); }

private:
    using Row = std::array<uint8_t, kRow>;
    std::map<uint32_t, Row> rows_;
};

}  // namespace cdmp
