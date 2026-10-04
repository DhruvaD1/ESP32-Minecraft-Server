#include "mc_packet.h"
#include "mc_types.h"
#include "lwip/sockets.h"
#include <algorithm>

bool PacketBuf::ensure(size_t additional) {
    if (len + additional <= cap_) return true;
    size_t new_cap = cap_ ? cap_ * 2 : 256;
    while (new_cap < len + additional) new_cap *= 2;
    auto bigger = psram_alloc<uint8_t>(new_cap);
    if (!bigger) {
        err = true;
        return false;
    }
    std::ranges::copy(bytes(), bigger.get());
    buf_ = std::move(bigger);
    cap_ = new_cap;
    return true;
}

void PacketBuf::append(std::span<const uint8_t> src) {
    if (!ensure(src.size())) return;
    std::ranges::copy(src, buf_.get() + len);
    len += src.size();
}

void PacketBuf::frame_into(PacketBuf& dst) const {
    pkt_write_varint(dst, static_cast<int32_t>(len));
    dst.append(bytes());
}

static bool recv_exact(int sock, std::span<uint8_t> out) {
    while (!out.empty()) {
        int r = recv(sock, out.data(), out.size(), 0);
        if (r <= 0) return false;
        out = out.subspan(r);
    }
    return true;
}

bool send_all(int sock, std::span<const uint8_t> data) {
    while (!data.empty()) {
        int r = send(sock, data.data(), data.size(), 0);
        if (r <= 0) return false;
        data = data.subspan(r);
    }
    return true;
}

bool PacketBuf::recv_packet(int sock) {
    reset();

    int32_t pkt_len;
    if (mc_read_varint_sock(sock, pkt_len) < 0 || pkt_len <= 0 || pkt_len > 65536)
        return false;

    if (!ensure(pkt_len)) return false;
    if (!recv_exact(sock, {buf_.get(), static_cast<size_t>(pkt_len)})) return false;
    len = pkt_len;
    return true;
}

bool PacketBuf::send_packet(int sock) {
    if (err) return false;
    auto hdr = encode_varint(static_cast<int32_t>(len));
    return send_all(sock, hdr.bytes()) && send_all(sock, bytes());
}
