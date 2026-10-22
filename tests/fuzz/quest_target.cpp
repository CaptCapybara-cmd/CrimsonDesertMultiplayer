// coopfuzz target for cdmp's quest sharing: stage sync (common/stage_sync.h) and quest accept
// sharing (common/quest_accept.h), with cdmp's real wire format (common/protocol.h) and reliable
// channel (common/reliable.h) over the fuzzer's lossy link. No game: each machine has a small
// fake world with the shape seen in Crimson Desert (NOTES.md 2026-10-20):
//   - R requests; each has an NPC (static key, a runtime id that changes on every load, loaded
//     only when the player is in its area), a meeting scene (a stage whose branches run 6, 0, 3, 1;
//     branch 6 makes the NPC known; a branch outside that list crashes the game), and mission rows
//     (main slot kind 2 -> 5 on accept, two objective rows added with kind 2; hand-in: main 5 -> 6,
//     the next mission's row added, and an item leaves the bags).
//   - the server checks what the real one is seen to check: an accept needs the NPC loaded and
//     known and the main slot at kind 2; a hand-in needs the dish.
// Machine 0 is the host. The glue between the queues and the game mirrors the mod
// (src/mod/net/stage_sync.cpp, quest_share.cpp, coop.cpp, common/client_session.cpp): queues are
// ticked every 50 ms, a loading screen resets runtime ids, a reconnection resets the reliable
// channels and the host then resends its story log (common/story_log.h). Where the mod's
// behaviour is the thing under test, it is noted.
//
// Ops: Activate = the player meets request a's NPC and accepts it (the host's is shared);
// Pickup = cooks and hands in request a (never shared); Move c = goes to area c (which NPCs are
// loaded); Drop = a loading screen on that machine; Hit = an unrelated mission row changes there;
// Join/Leave = the guest's link; Stale = an old shared message again; SetLink = the link;
// Spawn = the guest restarts its game (its world kept as saved, the mod starts fresh, then it
// reconnects); Save/Load = the host makes a save point / loads it (its world goes back, runtime
// ids change; the guest keeps what it has, a guest's progress is never rolled back).
//
// View (what must converge): for every request the HOST accepted, whether each machine has it
// accepted; for every scene the host finished, whether each machine's scene finished. A guest's
// own accepts and hand-ins are its own and are not compared.
// Native checks: a made-up branch reached a server (crash); the mod sent a hand-in.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

#include "coopfuzz/fuzz.h"

#include "common/protocol.h"
#include "common/quest_accept.h"
#include "common/reliable.h"
#include "common/stage_sync.h"
#include "common/story_log.h"
#include "common/story_digest.h"

#include "cofr.h"
#include <cstdlib>

namespace coopfuzz {
namespace {

constexpr int kRequests = 4;
constexpr int kAreas = 3;
constexpr uint32_t kChain[4] = {6, 0, 3, 1};
constexpr uint32_t kQuestKey = 0x0F4440;
constexpr uint32_t kAnswerAccept = 10, kAnswerHandIn = 11;
constexpr int kNoiseRows = 3;

uint32_t stage_key(int r) { return 0xF4700u + static_cast<uint32_t>(r); }
uint32_t npc_key(int r) { return 0x01C29010u + static_cast<uint32_t>(r); }
int npc_area(int r) { return r % kAreas; }
uint32_t main_key(int r) { return 0xFFFFF7F8u - 3u * static_cast<uint32_t>(r); }
uint32_t child_key(int r, int c) { return main_key(r) - 1u - static_cast<uint32_t>(c); }
uint32_t noise_key(int j) { return 0x0F5000u + static_cast<uint32_t>(j); }
uint32_t next_key(int r) { return 0x0F4480u + static_cast<uint32_t>(r); }  // the mission a hand-in opens next

struct Bugs {
    bool share_handins = false;   // the detector ignores item removal
    bool no_npc_translate = false;// the guest answers with the host's runtime NPC id
    bool no_chain = false;        // the stage queue only sends to an idle stage (before 2026-10-20)
    bool no_story_dedup = false;  // the guest applies every story entry, also the ones it already had
    bool clear_on_loading = false;// the guest drops its queues on a loading screen (before 2026-10-22)
};

// One machine: its fake game world and server, and the mod's queues.
struct Machine {
    int id = 0;
    uint32_t player = 0;
    int area = 0;
    uint32_t gen = 1;                 // load generation (runtime ids change)
    bool regrouped = false;           // after heal: every NPC is loaded
    struct Req {
        int pos = -1;                 // last chain index applied (-1 = idle)
        bool known = false;
        int main = 2, c0 = -1, c1 = -1, next = -1;
        bool dish = false;
    };
    Req req[kRequests];
    int noise[kNoiseRows] = {1, 1, 1};
    struct Snapshot { Req req[kRequests]; int noise[kNoiseRows]; bool valid = false; } save;
    int crashes = 0, mod_handins = 0;
    // The game's own scripted actions in flight: (time, kind, request, chain index).
    struct Pending { uint64_t at; int kind; int r; int idx; };
    std::vector<Pending> todo;
    // mod side
    cdmp::StageSyncQueue stages;
    cdmp::GuestAcceptQueue accepts;
    cdmp::AcceptDetector detector;
    cdmp::StoryReceiver story_rx;
    // Flight recorder (env CDMP_FLIGHT_DIR): what the mod records (mod/net/flight.cpp), per machine.
    std::unique_ptr<cofr::Recorder> rec;
    cdmp::StoryDigest shared;
    std::unique_ptr<cdmp::ReliableChannel> out;  // to the other side (host: one per guest, below)
    uint64_t last_answer_ms = 0;

    uint32_t instance(int r) const { return 0x100u + gen * 16u + static_cast<uint32_t>(r); }
    uint32_t npc_runtime(int r) const { return 0xB0100000u + gen * 64u + static_cast<uint32_t>(id) * 8u + static_cast<uint32_t>(r); }
    bool npc_loaded(int r) const { return regrouped || area == npc_area(r); }
    int request_of_instance(uint64_t inst) const {
        for (int r = 0; r < kRequests; ++r) if (instance(r) == inst) return r;
        return -1;
    }
    int request_of_npc(uint32_t rt) const {
        for (int r = 0; r < kRequests; ++r) if (npc_runtime(r) == rt && npc_loaded(r)) return r;
        return -1;
    }
    std::vector<cdmp::LocalStage> local_stages() const {
        std::vector<cdmp::LocalStage> v;
        for (int r = 0; r < kRequests; ++r) {
            const Req& q = req[r];
            cdmp::LocalStage s{instance(r), stage_key(r), 0, 0xFFFFFFFF, false};
            if (q.pos >= 0) {
                s.branch = kChain[q.pos];
                s.state = q.pos == 3 ? 0 : 1;  // the butcher shop's last branch leaves it idle (state 0) on branch 1
            }
            v.push_back(s);
        }
        return v;
    }
    int mission_kind(uint32_t key) const {
        for (int r = 0; r < kRequests; ++r) {
            if (key == main_key(r)) return req[r].main;
            if (key == child_key(r, 0)) return req[r].c0;
            if (key == child_key(r, 1)) return req[r].c1;
            if (key == next_key(r)) return req[r].next;
        }
        for (int j = 0; j < kNoiseRows; ++j) if (key == noise_key(j)) return noise[j];
        return -1;
    }
};

class QuestTarget : public TargetBase {
public:
    QuestTarget(const Scenario& s, Bugs b)
        : net_(s.machines, s.seed * 131 + 7), n_(s.machines), bugs_(b), log_(static_cast<uint32_t>(s.seed * 2654435761u) | 1u) {
        LinkParams link = s.link;
        if (!s.profile.empty()) linkProfile(s.profile, link);
        net_.setLink(link);
        for (int m = 0; m < n_; ++m) {
            auto mc = std::make_unique<Machine>();
            mc->id = m;
            mc->player = 0xA0100001u + static_cast<uint32_t>(m);
            mc->area = static_cast<int>((s.seed + static_cast<uint64_t>(m)) % kAreas);
            ms_.push_back(std::move(mc));
        }
        if (const char* dir = std::getenv("CDMP_FLIGHT_DIR")) {
            flight_dir_ = dir;
            for (auto& mc : ms_) {
                cofr::Config c;
                c.slot = static_cast<uint8_t>(mc->id);
                c.role = mc->id == 0 ? cofr::kRoleHost : cofr::kRoleClient;
                c.label = mc->id == 0 ? "host" : "guest" + std::to_string(mc->id);
                c.dir = dir;
                c.schemaJson = R"json({"game":"crimson-desert (cdmp fuzz)","digest_categories":{"0":"shared stage branches","1":"shared mission rows"}})json";
                mc->rec = std::make_unique<cofr::Recorder>(c);
                mc->rec->setClockSource([this] { return now_ * 1000; });
                mc->rec->setWallStart(1);
                for (int o = 0; o < n_; ++o) mc->rec->mapPeer(static_cast<uint32_t>(o), static_cast<uint8_t>(o));
            }
        }
        // Reliable channels: host <-> each guest, one per direction end.
        for (int g = 1; g < n_; ++g) {
            host_out_.push_back(make_channel(0, g));
            ms_[static_cast<size_t>(g)]->out = make_channel(g, 0);
        }
    }

    uint64_t now() const override { return now_; }
    int machines() const override { return n_; }
    bool connected(int m) const override { return net_.connected(m); }
    uint64_t repairs(int) const override { return 0; }

    void apply(const Op& op) override {
        if (frozen_) return;
        const int m = std::max(0, std::min(n_ - 1, op.m));
        Machine& mc = *ms_[static_cast<size_t>(m)];
        const int r = ((op.a % kRequests) + kRequests) % kRequests;
        switch (op.kind) {
            case OpKind::Activate: {  // walk to the NPC, meet it (scene), accept
                ++counters_["op_meet_accept"];
                mc.area = npc_area(r);
                for (int i = 0; i < 4; ++i) mc.todo.push_back({now_ + 200u * static_cast<uint64_t>(i), 0, r, i});
                mc.todo.push_back({now_ + 1000, 1, r, 0});
                break;
            }
            case OpKind::Pickup:  // cook and hand in
                ++counters_["op_handin"];
                mc.area = npc_area(r);
                mc.req[r].dish = true;
                mc.todo.push_back({now_ + 100, 2, r, 0});
                break;
            case OpKind::Move: mc.area = ((op.c % kAreas) + kAreas) % kAreas; ++counters_["op_move"]; break;
            case OpKind::Drop:  // a loading screen: runtime ids change; mirrors the mod's reaction to it
                ++counters_["op_loading"];
                ++mc.gen;
                on_loading(mc);
                break;
            case OpKind::Hit: {  // an unrelated mission row changes (local progress)
                ++counters_["op_noise"];
                const int j = ((op.b % kNoiseRows) + kNoiseRows) % kNoiseRows;
                const int before = mc.noise[j];
                mc.noise[j] = before == 1 ? 2 : 1;
                if (m == 0) mc.detector.on_mission(noise_key(j), static_cast<uint8_t>(before), static_cast<uint8_t>(mc.noise[j]),
                                                   static_cast<uint32_t>(now_));
                break;
            }
            case OpKind::Spawn:  // the guest restarts its game
                if (m == 0) break;
                ++counters_["op_guest_restart"];
                mc.stages = cdmp::StageSyncQueue();
                mc.accepts = cdmp::GuestAcceptQueue();
                mc.story_rx = cdmp::StoryReceiver();
                mc.shared = cdmp::StoryDigest();
                mc.todo.clear();
                ++mc.gen;
                reconnect(m);
                break;
            case OpKind::Save:
                ++counters_["op_host_save"];
                std::copy(std::begin(ms_[0]->req), std::end(ms_[0]->req), std::begin(ms_[0]->save.req));
                std::copy(std::begin(ms_[0]->noise), std::end(ms_[0]->noise), std::begin(ms_[0]->save.noise));
                ms_[0]->save.valid = true;
                break;
            case OpKind::Load: {
                Machine& h = *ms_[0];
                if (!h.save.valid) break;
                ++counters_["op_host_load"];
                std::copy(std::begin(h.save.req), std::end(h.save.req), std::begin(h.req));
                std::copy(std::begin(h.save.noise), std::end(h.save.noise), std::begin(h.noise));
                h.todo.clear();
                ++h.gen;
                break;
            }
            case OpKind::Leave:
                if (m != 0) { ++counters_["op_leave"]; net_.setConnected(m, false); }
                break;
            case OpKind::Join:
                if (m != 0 && !net_.connected(m)) { ++counters_["op_join"]; reconnect(m); }
                break;
            case OpKind::Stale: {  // an old story entry again, as fresh content (what a resend after a reconnect does)
                const auto& entries = log_.entries();
                if (m == 0 || entries.empty()) break;
                ++counters_["op_stale"];
                const int n = static_cast<int>(entries.size());
                const auto& old = entries[static_cast<size_t>(((op.b % n) + n) % n)];
                host_out_[static_cast<size_t>(m - 1)]->send(cdmp::proto::RelKind::Story, cdmp::proto::encode_story(old),
                                                            static_cast<uint32_t>(now_));
                break;
            }
            case OpKind::SetLink: {
                LinkParams p = net_.link();
                p.minLatencyMs = static_cast<uint32_t>(std::max(0, op.a));
                p.maxLatencyMs = static_cast<uint32_t>(std::max(op.a, op.b));
                p.loss = std::max(0, std::min(400, op.c)) / 1000.0;
                p.duplicate = std::max(0, std::min(200, op.d)) / 1000.0;
                net_.setLink(p);
                break;
            }
            default: break;
        }
    }

    void advance(uint64_t toMs) override {
        while (now_ < toMs) {
            now_ = std::min<uint64_t>(now_ + 10, toMs);
            net_.advance(now_);
            for (int m = 0; m < n_; ++m) {
                int from = 0;
                Bytes d;
                while (net_.poll(m, from, d)) on_datagram(m, from, d);
            }
            for (auto& mc : ms_) run_game(*mc);
            while (now_ >= next50_) {
                next50_ += 50;
                if (now_ >= next_digest_) {
                    next_digest_ += 5000;
                    for (auto& mc : ms_) record_digest(*mc);
                }
                for (auto& mc : ms_) mod_tick(*mc);
                for (auto& c : host_out_) c->tick(static_cast<uint32_t>(now_));
                for (int g = 1; g < n_; ++g) ms_[static_cast<size_t>(g)]->out->tick(static_cast<uint32_t>(now_));
            }
        }
    }

    void heal() override {
        frozen_ = true;
        net_.setLink(LinkParams());
        for (int g = 1; g < n_; ++g) if (!net_.connected(g)) reconnect(g);
        for (auto& mc : ms_) mc->regrouped = true;  // the players meet up: every NPC is loaded for everyone
    }

    bool quiescent() const override {
        if (!frozen_ || !net_.idle()) return false;
        for (const auto& c : host_out_) if (c->unacked_fragments()) return false;
        for (int g = 1; g < n_; ++g) {
            const Machine& mc = *ms_[static_cast<size_t>(g)];
            if (mc.out->unacked_fragments() || mc.accepts.pending()) return false;
        }
        for (const auto& mc : ms_) if (!mc->todo.empty()) return false;
        if (!ms_[0]->detector.idle()) return false;  // the host's detector window
        return true;
        // Note: held stage branches (a guest whose own game took a stage elsewhere) expire after
        // 2 minutes and are not waited for.
    }

    std::vector<Thing> view(int m) const override {
        std::vector<Thing> v;
        const Machine& host = *ms_[0];
        const Machine& mc = *ms_[static_cast<size_t>(m)];
        for (int r = 0; r < kRequests; ++r) {
            Thing t;
            t.netId = static_cast<uint32_t>(r + 1);
            t.type = 1;
            const bool host_accepted = host.req[r].main >= 5;
            t.hp = host_accepted ? (mc.req[r].main >= 5 ? 1 : 0) : 0;
            const bool host_scene = host.req[r].pos == 3;
            t.x = host_scene ? (mc.req[r].pos == 3 ? 1 : 0) : 0;
            t.y = host.req[r].known ? (mc.req[r].known ? 1 : 0) : 0;
            v.push_back(t);
        }
        return v;
    }
    uint64_t digest(int m, int cat) const override {
        uint64_t h = 1469598103934665603ull + static_cast<uint64_t>(cat);
        for (const auto& t : view(m)) { h ^= static_cast<uint64_t>(t.hp * 4 + t.x * 2 + t.y); h *= 1099511628211ull; }
        return h;
    }
    void markQuiescent() override {
        for (int g = 1; g < n_; ++g) {
            const auto& c = ms_[static_cast<size_t>(g)]->accepts.counters();
            const std::string m = "m" + std::to_string(g) + "_accepts_";
            counters_[m + "received"] = c.received;
            counters_[m + "sent"] = c.sent;
            counters_[m + "accepted"] = c.accepted;
            counters_[m + "already"] = c.already;
            counters_[m + "failed"] = c.failed;
            counters_[m + "expired"] = c.expired;
            counters_[m + "interrupted"] = c.interrupted;
            counters_[m + "retried"] = c.retried;
            counters_[m + "rel_refused"] = ms_[static_cast<size_t>(g)]->out->refused();
            counters_[m + "host_unacked"] = host_out_[static_cast<size_t>(g - 1)]->unacked_fragments();
            counters_[m + "connected"] = net_.connected(g);
        }
    }
    std::vector<Violation> nativeViolations() override {
        if (!flight_dir_.empty()) dump_flight();
        std::vector<Violation> v;
        for (const auto& mc : ms_) {
            if (mc->crashes) v.push_back({"stale_applied", "m" + std::to_string(mc->id) + ": " + std::to_string(mc->crashes) +
                                                                  " made-up stage branch(es) reached its server (crash)", now_});
            if (mc->mod_handins) v.push_back({"converge", "m" + std::to_string(mc->id) + ": the mod sent " +
                                                              std::to_string(mc->mod_handins) + " hand-in(s) (hand-ins are never shared)", now_});
        }
        return v;
    }

private:
    std::unique_ptr<cdmp::ReliableChannel> make_channel(int from, int to) {
        return std::make_unique<cdmp::ReliableChannel>([this, from, to](cdmp::proto::Body b) {
            cdmp::proto::Packet p;
            p.seq = ++seq_;
            p.send_ms = static_cast<uint32_t>(now_);
            p.body = std::move(b);
            auto bytes = cdmp::proto::encode(p);
            if (auto& r = ms_[static_cast<size_t>(from)]->rec) r->netOut(static_cast<uint32_t>(to), bytes.data(), bytes.size());
            net_.send(from, to, std::move(bytes));
        });
    }
    cdmp::ReliableChannel& channel_to(int from, int to) {
        return from == 0 ? *host_out_[static_cast<size_t>(to - 1)] : *ms_[static_cast<size_t>(from)]->out;
    }

    void reconnect(int g) {
        if (std::getenv("CDMP_FUZZ_TRACE")) std::fprintf(stderr, "t=%llu reconnect m%d\n", (unsigned long long)now_, g);
        // common/client_session.cpp: a reconnection resets the reliable channel on both sides.
        net_.setConnected(g, true);
        host_out_[static_cast<size_t>(g - 1)]->reset();
        ms_[static_cast<size_t>(g)]->out->reset();
        ++counters_["reconnect"];
        // Then the host sends its whole story log again (coop.cpp on a new link).
        for (const auto& e : log_.entries()) {
            host_out_[static_cast<size_t>(g - 1)]->send(cdmp::proto::RelKind::Story, cdmp::proto::encode_story(e),
                                                        static_cast<uint32_t>(now_));
            ++counters_["story_resent"];
        }
    }

    void on_loading(Machine& mc) {
        // The mod resets its runtime-id tables on a loading screen and keeps the queues: their
        // entries name stages and NPCs by static key. (Before 2026-10-22 it cleared them: twin.)
        if (bugs_.clear_on_loading) {
            mc.stages.clear();
            mc.accepts.clear();
        }
    }

    void on_datagram(int at, int from, const Bytes& d) {
        if (auto& r = ms_[static_cast<size_t>(at)]->rec) r->netIn(static_cast<uint32_t>(from), d.data(), d.size());
        const auto p = cdmp::proto::decode(d);
        if (!p) { ++counters_["datagram_bad"]; return; }
        cdmp::ReliableChannel& ch = channel_to(at, from);
        if (const auto* ack = std::get_if<cdmp::proto::RelAck>(&p->body)) {
            if (std::getenv("CDMP_FUZZ_TRACE")) std::fprintf(stderr, "t=%llu m%d <- m%d ACK id=%08x idx=%u unacked_before=%zu\n",
                                                             (unsigned long long)now_, at, from, ack->msg_id, ack->index, ch.unacked_fragments());
            ch.on_ack(*ack);
            return;
        }
        const auto* data = std::get_if<cdmp::proto::RelData>(&p->body);
        if (!data) return;
        ++counters_["m" + std::to_string(at) + "_reldata_in"];
        if (std::getenv("CDMP_FUZZ_TRACE")) {
            const auto got = ch.on_data(*data, static_cast<uint32_t>(now_));
            std::fprintf(stderr, "t=%llu m%d <- m%d stream=%08x id=%08x kind=%d delivered=%zu\n", (unsigned long long)now_, at, from,
                         data->stream, data->msg_id, (int)data->kind, got.size());
            for (const auto& msg : got) on_message(at, from, msg);
            return;
        }
        for (const auto& msg : ch.on_data(*data, static_cast<uint32_t>(now_))) on_message(at, from, msg);
    }

    void on_message(int at, int from, const cdmp::ReliableChannel::Message& msg) {
        if (at == 0 || from != 0) return;  // CoopLink::accepts: both kinds go host -> guest only
        Machine& mc = *ms_[static_cast<size_t>(at)];
        const uint32_t t = static_cast<uint32_t>(now_);
        if (msg.kind != cdmp::proto::RelKind::Story) return;
        const auto e = cdmp::proto::decode_story(msg.bytes);
        if (!e) { ++counters_["story_undecodable"]; return; }
        ++counters_["m" + std::to_string(at) + "_story_seq" + std::to_string(e->seq)];
        if (!mc.story_rx.accept(*e) && !bugs_.no_story_dedup) {
            ++counters_["story_duplicate_ignored"];
            return;
        }
        mc.shared.note(*e);
        if (mc.rec) {
            cofr::AppliedEvent a;
            a.seq = e->seq;
            a.epoch = e->log_id;
            a.effectTag = static_cast<uint8_t>(e->kind);
            const uint8_t k = static_cast<uint8_t>(e->kind);
            a.hash = cofr::hash64(e->bytes.data(), e->bytes.size(), cofr::hash64(&k, 1));
            mc.rec->applied(a);
        }
        if (e->kind == cdmp::proto::RelKind::StageBranch) {
            if (const auto b = cdmp::proto::decode_stage_branch(e->bytes)) mc.stages.push(*b, t);
        } else if (e->kind == cdmp::proto::RelKind::QuestAccept) {
            if (const auto a = cdmp::proto::decode_quest_accept(e->bytes)) mc.accepts.push(*a, t);
        }
    }

    // The machine's own game: scripted actions due now.
    void run_game(Machine& mc) {
        for (size_t i = 0; i < mc.todo.size();) {
            const auto p = mc.todo[i];
            if (p.at > now_) { ++i; continue; }
            mc.todo.erase(mc.todo.begin() + static_cast<ptrdiff_t>(i));
            if (p.kind == 0) {  // the game picks the scene's next branch
                Machine::Req& q = mc.req[p.r];
                if (q.pos != p.idx - 1) continue;  // the scene already went on (e.g. the mod applied it)
                if (server_branch(mc, mc.instance(p.r), kChain[p.idx]) && mc.id == 0) {
                    share(cdmp::proto::RelKind::StageBranch, cdmp::proto::encode_stage_branch({stage_key(p.r), kChain[p.idx], 0}));
                }
            } else {  // the game answers a dialog: accept (1) or hand-in (2)
                std::vector<uint8_t> payload(30, 0);
                const uint32_t rt = mc.npc_runtime(p.r), qk = kQuestKey, ans = p.kind == 1 ? kAnswerAccept : kAnswerHandIn;
                std::memcpy(payload.data(), &rt, 4);
                std::memcpy(payload.data() + 4, &qk, 4);
                std::memcpy(payload.data() + 8, &ans, 4);
                if (mc.id == 0) {
                    mc.detector.on_answer(payload, npc_key(p.r), static_cast<uint32_t>(now_));
                    mc.last_answer_ms = now_;
                }
                server_answer(mc, payload, false);
            }
        }
    }

    // The machine's server. Returns true when it took the branch.
    bool server_branch(Machine& mc, uint64_t inst, uint32_t branch) {
        const int r = mc.request_of_instance(inst);
        if (r < 0) { ++counters_["server_branch_no_stage"]; return false; }
        int idx = -1;
        for (int i = 0; i < 4; ++i) if (kChain[i] == branch) idx = i;
        if (idx < 0) { ++mc.crashes; return false; }
        Machine::Req& q = mc.req[r];
        // A finished scene starts again on its first branch (a revisit); anything else out of order is refused.
        if (q.pos == 3 && idx == 0) {
            q.pos = 0;
            ++counters_["server_scene_restarted"];
            return true;
        }
        if (idx != q.pos + 1) { ++counters_["server_branch_out_of_order"]; return false; }
        q.pos = idx;
        if (idx == 0) q.known = true;
        return true;
    }

    void mission_changed(Machine& mc, uint32_t key, int before, int after) {
        if (mc.id == 0) mc.detector.on_mission(key, static_cast<uint8_t>(before < 0 ? 0 : before), static_cast<uint8_t>(after),
                                                static_cast<uint32_t>(now_));
    }

    // Returns true when the server did what the answer asked.
    bool server_answer(Machine& mc, const std::vector<uint8_t>& payload, bool from_mod) {
        uint32_t rt = 0, ans = 0;
        std::memcpy(&rt, payload.data(), 4);
        std::memcpy(&ans, payload.data() + 8, 4);
        const int r = mc.request_of_npc(rt);
        if (r < 0) { ++counters_[from_mod ? "server_mod_answer_no_npc" : "server_answer_no_npc"]; return false; }
        Machine::Req& q = mc.req[r];
        if (ans == kAnswerAccept) {
            if (!q.known || q.main != 2) { ++counters_["server_accept_refused"]; return false; }
            q.main = 5;
            mission_changed(mc, main_key(r), 2, 5);
            q.c0 = 2;
            mission_changed(mc, child_key(r, 0), -1, 2);
            q.c1 = 2;
            mission_changed(mc, child_key(r, 1), -1, 2);
            ++counters_[from_mod ? "accept_by_mod" : "accept_by_game"];
            return true;
        }
        if (ans == kAnswerHandIn) {
            if (q.main != 5 || !q.dish) { ++counters_["server_handin_refused"]; return false; }
            q.main = 6;
            q.dish = false;
            mission_changed(mc, main_key(r), 5, 6);
            // Like the real hand-in (2026-10-20 15:15): the next missions appear too, so only the
            // item removal tells it from an accept.
            if (q.next < 0) {
                q.next = 2;
                mission_changed(mc, next_key(r), -1, 2);
            }
            // Twin share_handins: the detector never hears of the removal.
            if (mc.id == 0 && !bugs_.share_handins) mc.detector.on_item_removed(static_cast<uint32_t>(now_));
            ++counters_["handin"];
            return true;
        }
        return false;
    }

    void share(cdmp::proto::RelKind kind, std::vector<uint8_t> bytes) {
        const auto e = log_.append(kind, std::move(bytes));
        Machine& host = *ms_[0];
        host.shared.note(e);
        if (host.rec) {
            cofr::CapturedEvent c;
            c.seq = e.seq;
            c.epoch = e.log_id;
            c.effectCount = 1;
            const uint8_t k = static_cast<uint8_t>(e.kind);
            c.hash = cofr::hash64(e.bytes.data(), e.bytes.size(), cofr::hash64(&k, 1));
            host.rec->captured(c);
        }
        const auto w = cdmp::proto::encode_story(e);
        for (int g = 1; g < n_; ++g) host_out_[static_cast<size_t>(g - 1)]->send(cdmp::proto::RelKind::Story, w, static_cast<uint32_t>(now_));
    }

    // The mod's co-op tick (coop.cpp, every 50 ms): stage sync, then quest share.
    void mod_tick(Machine& mc) {
        const uint32_t t = static_cast<uint32_t>(now_);
        if (mc.id == 0) {
            // The host's detector sees its own answers' outcome once the window is over.
            while (auto a = mc.detector.tick(t)) share(cdmp::proto::RelKind::QuestAccept, cdmp::proto::encode_quest_accept(*a));
            return;
        }
        // Guest: the host's branches onto our own instances.
        auto local = mc.local_stages();
        if (bugs_.no_chain) {
            // Before 2026-10-20 the queue only sent to an idle stage: emulate by hiding busy stages.
            for (auto& l : local) if (l.state != 0) l.busy = true;
        }
        for (const auto& act : mc.stages.tick(local, t)) {
            // idem 0: a (key, branch) pair legitimately repeats (a scene played again); the view checks the outcome.
            logApply(mc.id, 0, static_cast<int>(act.key), 0, act.branch, 0, ApplyKind::Intent, true, 0, "stage branch sent");
            server_branch(mc, act.instance, act.branch);
        }
        // Guest: the host's accepts.
        cdmp::GuestAcceptQueue::World w;
        w.npc_runtime = [&mc](uint32_t key) {
            for (int r = 0; r < kRequests; ++r) if (npc_key(r) == key) return mc.npc_loaded(r) ? mc.npc_runtime(r) : 0u;
            return 0u;
        };
        w.mission_kind = [&mc](uint32_t key) { return mc.mission_kind(key); };
        w.own_player = mc.player;
        for (auto& d : mc.accepts.tick(w, t)) {
            if (d.op != cdmp::GuestAcceptQueue::kOpQuestDialogEndReq) continue;  // obstacles: no state here
            if (bugs_.no_npc_translate) {
                // The host's own runtime id for that NPC instead of ours.
                uint32_t rt = 0;
                std::memcpy(&rt, d.payload.data(), 4);
                for (int r = 0; r < kRequests; ++r) if (mc.npc_runtime(r) == rt) { const uint32_t h = ms_[0]->npc_runtime(r); std::memcpy(d.payload.data(), &h, 4); }
            }
            uint32_t ans = 0;
            std::memcpy(&ans, d.payload.data() + 8, 4);
            if (ans == kAnswerHandIn) ++mc.mod_handins;
            const bool ok = server_answer(mc, d.payload, true);
            uint32_t rt = 0;
            std::memcpy(&rt, d.payload.data(), 4);
            logApply(mc.id, (2ull << 60) | (static_cast<uint64_t>(mc.id) << 32) | rt, -1, 0, ans, 0, ApplyKind::Intent, ok, 0,
                     ok ? "accept sent" : "accept refused by the server");
        }
    }

    void record_digest(Machine& mc) {
        if (!mc.rec || mc.shared.empty()) return;
        const uint32_t seq = mc.id == 0 ? (log_.entries().empty() ? 0 : log_.entries().back().seq) : mc.story_rx.last();
        const uint32_t epoch = mc.id == 0 ? log_.id() : mc.story_rx.id();
        const auto local = mc.local_stages();
        const std::vector<cdmp::StoryDigest::Entry> cats[2] = {
            mc.shared.stages([&](uint32_t key) -> std::optional<std::pair<uint32_t, uint32_t>> {
                for (const auto& l : local) if (l.key == key) return std::make_pair(l.state, l.branch);
                return std::nullopt;
            }),
            mc.shared.missions([&mc](uint32_t key) { return mc.mission_kind(key); }),
        };
        for (uint8_t c = 0; c < 2; ++c) {
            cofr::BucketedState st;
            for (const auto& e : cats[c]) st.set(e.key, e.value_hash);
            mc.rec->digest(st.digest(c, epoch, seq), cofr::kFlagBucketed);
            mc.rec->digestDetail(c, epoch, seq, st.entries());
            for (const auto& e : cats[c]) mc.rec->entryText(e.key, e.value_hash, e.text);
        }
    }

    void dump_flight() {
        for (auto& mc : ms_) {
            if (!mc->rec) continue;
            record_digest(*mc);
            mc->rec->dump(flight_dir_ + "/" + mc->rec->config().label + ".cofr", cofr::kDumpTest);
        }
    }

    LossyLink net_;
    int n_;
    std::string flight_dir_;
    uint64_t next_digest_ = 5000;
    Bugs bugs_;
    bool frozen_ = false;
    uint64_t now_ = 0, next50_ = 50;
    uint32_t seq_ = 0;
    std::vector<std::unique_ptr<Machine>> ms_;
    std::vector<std::unique_ptr<cdmp::ReliableChannel>> host_out_;
    cdmp::StoryLog log_;
};

std::unique_ptr<ITarget> makeQuests(const Scenario& s, std::string* err) {
    Bugs b;
    for (const auto& n : s.bugs) {
        if (n == "share_handins") b.share_handins = true;
        else if (n == "no_npc_translate") b.no_npc_translate = true;
        else if (n == "no_chain") b.no_chain = true;
        else if (n == "no_story_dedup") b.no_story_dedup = true;
        else if (n == "clear_on_loading") b.clear_on_loading = true;
        else { if (err) *err = "unknown bug class for target cdmp_quests: " + n; return nullptr; }
    }
    return std::make_unique<QuestTarget>(s, b);
}

const TargetRegistrar kRegisterQuests(TargetInfo{"cdmp_quests",
                                                 "cdmp stage sync + quest accept sharing over its reliable channel (fake Crimson Desert world)",
                                                 {"share_handins", "no_npc_translate", "no_chain", "no_story_dedup",
                                                  "clear_on_loading"},
                                                 makeQuests});

}  // namespace
}  // namespace coopfuzz
