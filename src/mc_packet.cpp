#include "mc_packet.h"
#include "mc_types.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include <cstring>

bool PacketBuf::init(size_t initial_cap) {
    data = static_cast<uint8_t*>(heap_caps_malloc(initial_cap, MALLOC_CAP_SPIRAM));
    cap = data ? initial_cap : 0;
    len = 0;
    pos = 0;
    err = (data == nullptr);
    return data != nullptr;
}

void PacketBuf::free() {
    if (data) { heap_caps_free(data); data = nullptr; }
    cap = len = pos = 0;
}

void PacketBuf::reset() {
    len = 0;
    pos = 0;
    err = false;
}

bool PacketBuf::ensure(size_t additional) {
    if (len + additional <= cap) return true;
    size_t new_cap = cap ? cap * 2 : 256;
    while (new_cap < len + additional) new_cap *= 2;
    auto* new_buf = static_cast<uint8_t*>(heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM));
    if (!new_buf) { err = true; return false; }
    if (data) {
        std::memcpy(new_buf, data, len);
        heap_caps_free(data);
    }
    data = new_buf;
    cap = new_cap;
    return true;
}

void PacketBuf::append(const uint8_t* src, size_t n) {
    if (!ensure(n)) return;
    std::memcpy(data + len, src, n);
    len += n;
}

void PacketBuf::frame_into(PacketBuf& dst) const {
    uint8_t hdr[5];
    int hdr_len = mc_write_varint(hdr, static_cast<int32_t>(len));
    dst.append(hdr, hdr_len);
    dst.append(data, len);
}

static bool recv_exact(int sock, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(sock, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool send_all(int sock, const uint8_t* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = send(sock, data + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

bool PacketBuf::recv_packet(int sock) {
    reset();

    int32_t pkt_len;
    if (mc_read_varint_sock(sock, pkt_len) < 0 || pkt_len <= 0 || pkt_len > 65536)
        return false;

    if (!ensure(pkt_len)) return false;
    if (!recv_exact(sock, data, pkt_len)) return false;
    len = pkt_len;
    pos = 0;
    return true;
}

bool PacketBuf::send_packet(int sock) {
    if (err) return false;
    uint8_t hdr[5];
    int hdr_len = mc_write_varint(hdr, static_cast<int32_t>(len));
    return send_all(sock, hdr, hdr_len) && send_all(sock, data, len);
}
