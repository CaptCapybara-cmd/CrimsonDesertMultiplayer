#pragma once
// Datagram transport behind the session: plain UDP (development on one PC,
// or, for testing only, two PCs on one Tailscale tailnet: [dev] udp_tailnet) or Steam networking (no port
// forwarding; src/mod/net/steam_transport.h). Datagrams are at most
// proto::kMaxPacketSize bytes, unreliable and unordered on both.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "common/udp.h"

namespace cdmp::net {

// Address of the other player, whatever the transport. Never a memory address.
struct PeerAddr {
    enum class Kind : uint8_t { None, Udp, Steam };
    Kind kind = Kind::None;
    uint64_t value = 0;  // Udp: ip (network order) << 16 | port   Steam: SteamID64

    static PeerAddr udp(const Endpoint& ep) {
        return {Kind::Udp, (static_cast<uint64_t>(ep.ip) << 16) | ep.port};
    }
    static PeerAddr steam(uint64_t steam_id) { return {Kind::Steam, steam_id}; }
    Endpoint endpoint() const {
        return {static_cast<uint32_t>(value >> 16), static_cast<uint16_t>(value & 0xFFFF)};
    }
    bool valid() const { return kind != Kind::None; }
    std::string str() const {
        if (kind == Kind::Udp) return "udp:" + endpoint().str();
        if (kind == Kind::Steam) return "steam:" + std::to_string(value);
        return "none";
    }
    bool operator==(const PeerAddr&) const = default;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(const PeerAddr& to, std::span<const uint8_t> data) = 0;
    // Waits up to timeout_ms for one datagram; its size, or nullopt.
    virtual std::optional<size_t> recv(PeerAddr& from, std::span<uint8_t> buf, int timeout_ms) = 0;
    // Periodic housekeeping (accept sessions, status). Cheap; call every loop.
    virtual void poll(uint32_t /*now_ms*/) {}
    // False while it cannot send yet (e.g. Steam not logged on).
    virtual bool ready() const { return true; }
    virtual std::string status() const = 0;
};

class UdpTransport final : public Transport {
public:
    // port 0 = ephemeral (a guest); a host listens on a fixed port.
    // loopback_only keeps a single-PC test off the LAN and the firewall.
    bool open(uint16_t port, bool loopback_only) {
        if (!sock_.open(port, loopback_only)) return false;
        loopback_only_ = loopback_only;
        return true;
    }
    // Tailnet test mode: bound to this PC's Tailscale address, and datagrams from
    // outside 100.64.0.0/10 are dropped (a host locks onto whoever speaks first).
    bool open_tailnet(uint16_t port, uint32_t tailnet_ip) {
        if (!sock_.open_at(port, tailnet_ip)) return false;
        loopback_only_ = false;
        tailnet_ip_ = tailnet_ip;
        return true;
    }
    bool send(const PeerAddr& to, std::span<const uint8_t> data) override {
        return to.kind == PeerAddr::Kind::Udp && sock_.send_to(to.endpoint(), data);
    }
    std::optional<size_t> recv(PeerAddr& from, std::span<uint8_t> buf, int timeout_ms) override {
        Endpoint ep;
        auto n = sock_.recv_from(ep, buf, timeout_ms);
        if (n && tailnet_ip_ && !ep.is_tailnet()) {
            ++dropped_;
            return std::nullopt;
        }
        if (n) from = PeerAddr::udp(ep);
        return n;
    }
    std::string status() const override {
        if (tailnet_ip_) {
            return "udp " + Endpoint{tailnet_ip_, sock_.local_port()}.str() + " (tailnet only, " +
                   std::to_string(dropped_) + " outside datagrams dropped)";
        }
        return "udp port " + std::to_string(sock_.local_port()) +
               (loopback_only_ ? " (loopback only)" : " (all interfaces)");
    }

private:
    UdpSocket sock_;
    bool loopback_only_ = true;
    uint32_t tailnet_ip_ = 0;  // network order; 0 = not in tailnet mode
    uint64_t dropped_ = 0;
};

}  // namespace cdmp::net
