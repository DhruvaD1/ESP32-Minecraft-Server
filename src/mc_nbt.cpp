#include "mc_nbt.h"
#include "mc_types.h"
#include <cstring>

static void write_name(PacketBuf& b, const char* name) {
    auto len = static_cast<uint16_t>(strlen(name));
    uint8_t tmp[2];
    mc_write_u16(tmp, len);
    b.append(tmp, 2);
    b.append(reinterpret_cast<const uint8_t*>(name), len);
}

static void tag_header(PacketBuf& b, uint8_t type, const char* name) {
    pkt_write_byte(b, type);
    write_name(b, name);
}

void nbt_begin(PacketBuf& b) {
    pkt_write_byte(b, 0x0A);
}

void nbt_end(PacketBuf& b) {
    pkt_write_byte(b, 0x00);
}

void nbt_compound(PacketBuf& b, const char* name) {
    tag_header(b, 0x0A, name);
}

void nbt_byte(PacketBuf& b, const char* name, int8_t val) {
    tag_header(b, 0x01, name);
    pkt_write_byte(b, static_cast<uint8_t>(val));
}

void nbt_int(PacketBuf& b, const char* name, int32_t val) {
    tag_header(b, 0x03, name);
    uint8_t tmp[4]; mc_write_i32(tmp, val); b.append(tmp, 4);
}

void nbt_long(PacketBuf& b, const char* name, int64_t val) {
    tag_header(b, 0x04, name);
    uint8_t tmp[8]; mc_write_i64(tmp, val); b.append(tmp, 8);
}

void nbt_float(PacketBuf& b, const char* name, float val) {
    tag_header(b, 0x05, name);
    uint8_t tmp[4]; mc_write_f32(tmp, val); b.append(tmp, 4);
}

void nbt_double(PacketBuf& b, const char* name, double val) {
    tag_header(b, 0x06, name);
    uint8_t tmp[8]; mc_write_f64(tmp, val); b.append(tmp, 8);
}

void nbt_string(PacketBuf& b, const char* name, const char* val) {
    tag_header(b, 0x08, name);
    auto len = static_cast<uint16_t>(strlen(val));
    uint8_t tmp[2]; mc_write_u16(tmp, len); b.append(tmp, 2);
    b.append(reinterpret_cast<const uint8_t*>(val), len);
}

void nbt_string_list(PacketBuf& b, const char* name, const char** items, int count) {
    tag_header(b, 0x09, name);
    pkt_write_byte(b, 0x08);
    uint8_t tmp[4]; mc_write_i32(tmp, count); b.append(tmp, 4);
    for (int i = 0; i < count; i++) {
        auto len = static_cast<uint16_t>(strlen(items[i]));
        mc_write_u16(tmp, len); b.append(tmp, 2);
        b.append(reinterpret_cast<const uint8_t*>(items[i]), len);
    }
}

void nbt_long_array(PacketBuf& b, const char* name, const int64_t* vals, int32_t count) {
    tag_header(b, 0x0C, name);
    uint8_t tmp[8]; mc_write_i32(tmp, count); b.append(tmp, 4);
    for (int32_t i = 0; i < count; i++) {
        mc_write_i64(tmp, vals[i]); b.append(tmp, 8);
    }
}
