#include "mc_nbt.h"
#include "mc_types.h"

enum class Tag : uint8_t { End = 0, Byte = 1, Int = 3, Long = 4, Float = 5, Double = 6, String = 8, List = 9, Compound = 10, LongArray = 12 };

static void write_nbt_string(PacketBuf& b, std::string_view s) {
    pkt_write_u16(b, static_cast<uint16_t>(s.size()));
    b.append({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
}

static void tag_header(PacketBuf& b, Tag type, std::string_view name) {
    pkt_write_byte(b, std::to_underlying(type));
    write_nbt_string(b, name);
}

void nbt_begin(PacketBuf& b) { pkt_write_byte(b, std::to_underlying(Tag::Compound)); }
void nbt_end(PacketBuf& b) { pkt_write_byte(b, std::to_underlying(Tag::End)); }

void nbt_compound(PacketBuf& b, std::string_view name) { tag_header(b, Tag::Compound, name); }

void nbt_byte(PacketBuf& b, std::string_view name, int8_t val) {
    tag_header(b, Tag::Byte, name);
    pkt_write(b, val);
}

void nbt_int(PacketBuf& b, std::string_view name, int32_t val) {
    tag_header(b, Tag::Int, name);
    pkt_write(b, val);
}

void nbt_long(PacketBuf& b, std::string_view name, int64_t val) {
    tag_header(b, Tag::Long, name);
    pkt_write(b, val);
}

void nbt_float(PacketBuf& b, std::string_view name, float val) {
    tag_header(b, Tag::Float, name);
    pkt_write(b, val);
}

void nbt_double(PacketBuf& b, std::string_view name, double val) {
    tag_header(b, Tag::Double, name);
    pkt_write(b, val);
}

void nbt_string(PacketBuf& b, std::string_view name, std::string_view val) {
    tag_header(b, Tag::String, name);
    write_nbt_string(b, val);
}

void nbt_string_list(PacketBuf& b, std::string_view name, std::span<const std::string_view> items) {
    tag_header(b, Tag::List, name);
    pkt_write_byte(b, std::to_underlying(Tag::String));
    pkt_write_i32(b, static_cast<int32_t>(items.size()));
    for (auto item : items) write_nbt_string(b, item);
}

void nbt_long_array(PacketBuf& b, std::string_view name, std::span<const int64_t> vals) {
    tag_header(b, Tag::LongArray, name);
    pkt_write_i32(b, static_cast<int32_t>(vals.size()));
    for (int64_t v : vals) pkt_write(b, v);
}
