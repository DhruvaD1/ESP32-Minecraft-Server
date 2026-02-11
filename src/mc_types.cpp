#include "mc_types.h"
#include "mc_packet.h"
#include <cstring>
#include "lwip/sockets.h"

int mc_read_varint(const uint8_t* buf, size_t buf_len, int32_t& out_value) {
    out_value = 0;
    int shift = 0;
    for (size_t i = 0; i < buf_len && i < 5; i++) {
        out_value |= (static_cast<int32_t>(buf[i] & 0x7F)) << shift;
        if ((buf[i] & 0x80) == 0) return i + 1;
        shift += 7;
    }
    return -1;
}

int mc_write_varint(uint8_t* buf, int32_t value) {
    auto uval = static_cast<uint32_t>(value);
    int i = 0;
    do {
        uint8_t b = uval & 0x7F;
        uval >>= 7;
        if (uval) b |= 0x80;
        buf[i++] = b;
    } while (uval);
    return i;
}

int mc_varint_size(int32_t value) {
    auto uval = static_cast<uint32_t>(value);
    int size = 0;
    do { uval >>= 7; size++; } while (uval);
    return size;
}

int mc_read_varint_sock(int sock, int32_t& out_value) {
    out_value = 0;
    int shift = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t b;
        int r = recv(sock, &b, 1, 0);
        if (r <= 0) return -1;
        out_value |= (static_cast<int32_t>(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) return i + 1;
        shift += 7;
    }
    return -1;
}

uint16_t mc_read_u16(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

int16_t mc_read_i16(const uint8_t* buf) {
    return static_cast<int16_t>(mc_read_u16(buf));
}

int32_t mc_read_i32(const uint8_t* buf) {
    return (static_cast<int32_t>(buf[0]) << 24) |
           (static_cast<int32_t>(buf[1]) << 16) |
           (static_cast<int32_t>(buf[2]) << 8)  | buf[3];
}

int64_t mc_read_i64(const uint8_t* buf) {
    return (static_cast<int64_t>(mc_read_i32(buf)) << 32) |
           (static_cast<uint32_t>(mc_read_i32(buf + 4)));
}

float mc_read_f32(const uint8_t* buf) {
    int32_t i = mc_read_i32(buf);
    float f;
    std::memcpy(&f, &i, 4);
    return f;
}

double mc_read_f64(const uint8_t* buf) {
    int64_t i = mc_read_i64(buf);
    double d;
    std::memcpy(&d, &i, 8);
    return d;
}

void mc_write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = val >> 8;
    buf[1] = val & 0xFF;
}

void mc_write_i16(uint8_t* buf, int16_t val) {
    mc_write_u16(buf, static_cast<uint16_t>(val));
}

void mc_write_i32(uint8_t* buf, int32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] = val & 0xFF;
}

void mc_write_i64(uint8_t* buf, int64_t val) {
    mc_write_i32(buf, static_cast<int32_t>(val >> 32));
    mc_write_i32(buf + 4, static_cast<int32_t>(val));
}

void mc_write_f32(uint8_t* buf, float val) {
    int32_t i;
    std::memcpy(&i, &val, 4);
    mc_write_i32(buf, i);
}

void mc_write_f64(uint8_t* buf, double val) {
    int64_t i;
    std::memcpy(&i, &val, 8);
    mc_write_i64(buf, i);
}

int64_t mc_encode_position(int x, int y, int z) {
    return ((static_cast<int64_t>(x) & 0x3FFFFFF) << 38) |
           ((static_cast<int64_t>(z) & 0x3FFFFFF) << 12) |
           (static_cast<int64_t>(y) & 0xFFF);
}

void mc_decode_position(int64_t val, int& x, int& y, int& z) {
    x = static_cast<int>(val >> 38);
    z = static_cast<int>((val >> 12) & 0x3FFFFFF);
    y = static_cast<int>(val & 0xFFF);
    if (x >= (1 << 25)) x -= (1 << 26);
    if (z >= (1 << 25)) z -= (1 << 26);
    if (y >= (1 << 11)) y -= (1 << 12);
}

void pkt_write_byte(PacketBuf& b, uint8_t val) { b.append(&val, 1); }
void pkt_write_bool(PacketBuf& b, bool val) { uint8_t v = val ? 1 : 0; b.append(&v, 1); }

void pkt_write_u16(PacketBuf& b, uint16_t val) {
    uint8_t tmp[2]; mc_write_u16(tmp, val); b.append(tmp, 2);
}
void pkt_write_i16(PacketBuf& b, int16_t val) {
    uint8_t tmp[2]; mc_write_i16(tmp, val); b.append(tmp, 2);
}
void pkt_write_i32(PacketBuf& b, int32_t val) {
    uint8_t tmp[4]; mc_write_i32(tmp, val); b.append(tmp, 4);
}
void pkt_write_i64(PacketBuf& b, int64_t val) {
    uint8_t tmp[8]; mc_write_i64(tmp, val); b.append(tmp, 8);
}
void pkt_write_f32(PacketBuf& b, float val) {
    uint8_t tmp[4]; mc_write_f32(tmp, val); b.append(tmp, 4);
}
void pkt_write_f64(PacketBuf& b, double val) {
    uint8_t tmp[8]; mc_write_f64(tmp, val); b.append(tmp, 8);
}
void pkt_write_varint(PacketBuf& b, int32_t val) {
    uint8_t tmp[5]; int n = mc_write_varint(tmp, val); b.append(tmp, n);
}
void pkt_write_string(PacketBuf& b, const char* str) {
    auto slen = static_cast<int32_t>(strlen(str));
    pkt_write_varint(b, slen);
    b.append(reinterpret_cast<const uint8_t*>(str), slen);
}
void pkt_write_position(PacketBuf& b, int x, int y, int z) {
    pkt_write_i64(b, mc_encode_position(x, y, z));
}
void pkt_write_uuid(PacketBuf& b, uint64_t hi, uint64_t lo) {
    pkt_write_i64(b, static_cast<int64_t>(hi));
    pkt_write_i64(b, static_cast<int64_t>(lo));
}

uint8_t pkt_read_byte(PacketBuf& b) { return b.data[b.pos++]; }
bool pkt_read_bool(PacketBuf& b) { return pkt_read_byte(b) != 0; }

uint16_t pkt_read_u16(PacketBuf& b) {
    uint16_t v = mc_read_u16(b.data + b.pos); b.pos += 2; return v;
}
int16_t pkt_read_i16(PacketBuf& b) {
    int16_t v = mc_read_i16(b.data + b.pos); b.pos += 2; return v;
}
int32_t pkt_read_i32(PacketBuf& b) {
    int32_t v = mc_read_i32(b.data + b.pos); b.pos += 4; return v;
}
int64_t pkt_read_i64(PacketBuf& b) {
    int64_t v = mc_read_i64(b.data + b.pos); b.pos += 8; return v;
}
float pkt_read_f32(PacketBuf& b) {
    float v = mc_read_f32(b.data + b.pos); b.pos += 4; return v;
}
double pkt_read_f64(PacketBuf& b) {
    double v = mc_read_f64(b.data + b.pos); b.pos += 8; return v;
}
int32_t pkt_read_varint(PacketBuf& b) {
    int32_t val;
    int n = mc_read_varint(b.data + b.pos, b.len - b.pos, val);
    if (n > 0) b.pos += n;
    return val;
}
size_t pkt_read_string(PacketBuf& b, char* out, size_t max_len) {
    int32_t slen = pkt_read_varint(b);
    size_t copy = (static_cast<size_t>(slen) < max_len - 1) ? slen : max_len - 1;
    std::memcpy(out, b.data + b.pos, copy);
    out[copy] = '\0';
    b.pos += slen;
    return copy;
}
void pkt_read_position(PacketBuf& b, int& x, int& y, int& z) {
    int64_t v = pkt_read_i64(b);
    mc_decode_position(v, x, y, z);
}
void pkt_read_uuid(PacketBuf& b, uint64_t& hi, uint64_t& lo) {
    hi = static_cast<uint64_t>(pkt_read_i64(b));
    lo = static_cast<uint64_t>(pkt_read_i64(b));
}
