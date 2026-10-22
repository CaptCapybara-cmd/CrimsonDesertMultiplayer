#pragma once
// The host's story log: every quest stage branch and quest accept the host shares
// (stage_sync.h, quest_accept.h) gets a dense sequence number in a log with a random
// id (one per host session). After a (re)connection the host sends the whole log
// again; the guest applies each number once and ignores the rest (coop-anything
// playbook/effect-capture-story-sync.md, section 5).
// Why: a reconnection resets the reliable channels (client_session.cpp), so what was
// in flight was lost, and resending without numbers re-runs scenes on the guest (a
// scene's branch again is a legitimate new branch to the guest's queue). Found by the
// fuzzer (tests/fuzz): a guest who dropped out mid-scene never got the request.
// No game or network in here; the tests drive it.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/protocol.h"

namespace cdmp {

class StoryLog {
public:
    static constexpr size_t kMaxEntries = 4096;  // a session's shared story; beyond it, oldest dropped
    explicit StoryLog(uint32_t id) : id_(id ? id : 1) {}

    proto::StoryEntry append(proto::RelKind kind, std::vector<uint8_t> bytes) {
        proto::StoryEntry e{id_, ++seq_, kind, std::move(bytes)};
        entries_.push_back(e);
        if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin());
        return e;
    }
    const std::vector<proto::StoryEntry>& entries() const { return entries_; }
    uint32_t id() const { return id_; }

private:
    uint32_t id_;
    uint32_t seq_ = 0;
    std::vector<proto::StoryEntry> entries_;
};

class StoryReceiver {
public:
    // True when this entry is new here (apply it); false for one already applied.
    // A new log id (the host restarted) starts over.
    bool accept(const proto::StoryEntry& e) {
        if (e.log_id != id_) {
            id_ = e.log_id;
            last_ = 0;
        }
        if (e.seq <= last_) {
            ++duplicates_;
            return false;
        }
        last_ = e.seq;
        return true;
    }
    uint32_t last() const { return last_; }
    uint32_t id() const { return id_; }
    uint64_t duplicates() const { return duplicates_; }

private:
    uint32_t id_ = 0, last_ = 0;
    uint64_t duplicates_ = 0;
};

}  // namespace cdmp
