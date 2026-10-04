#include "mc_types.h"
#include "lwip/sockets.h"

static_assert(encode_varint(0).size == 1 && encode_varint(0).buf[0] == 0x00);
static_assert(encode_varint(300).size == 2 && encode_varint(300).buf[0] == 0xAC && encode_varint(300).buf[1] == 0x02);
static_assert(encode_varint(-1).size == 5 && encode_varint(-1).buf[4] == 0x0F);
static_assert(to_be<int32_t>(0x01020304) == std::array<uint8_t, 4>{1, 2, 3, 4});
static_assert(to_be<float>(1.0f) == std::array<uint8_t, 4>{0x3F, 0x80, 0x00, 0x00});
static_assert(from_be<int16_t>(std::array<uint8_t, 2>{0xFF, 0xFE}) == -2);
static_assert(encode_position(-1, -64, 1) == static_cast<int64_t>(0xFFFFFFC000001FC0ULL));

int mc_read_varint_sock(int sock, int32_t& out_value) {
    std::array<uint8_t, 5> buf;
    for (size_t i = 0; i < buf.size(); i++) {
        if (recv(sock, &buf[i], 1, 0) <= 0) return -1;
        if ((buf[i] & 0x80) == 0) return decode_varint(std::span(buf).first(i + 1), out_value);
    }
    return -1;
}

void pkt_write_string(PacketBuf& b, std::string_view s) {
    pkt_write_varint(b, static_cast<int32_t>(s.size()));
    b.append({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
}

int32_t pkt_read_varint(PacketBuf& b) {
    if (b.err) return 0;
    int32_t val = 0;
    int n = decode_varint(b.unread(), val);
    if (n < 0) {
        b.err = true;
        b.pos = b.len;
        return 0;
    }
    b.pos += n;
    return val;
}

std::string_view pkt_read_string(PacketBuf& b, size_t max_bytes) {
    int32_t slen = pkt_read_varint(b);
    if (b.err || slen < 0 || static_cast<size_t>(slen) > max_bytes ||
        static_cast<size_t>(slen) > b.unread().size()) {
        b.err = true;
        b.pos = b.len;
        return {};
    }
    std::string_view s(reinterpret_cast<const char*>(b.unread().data()), slen);
    b.pos += slen;
    return s;
}
