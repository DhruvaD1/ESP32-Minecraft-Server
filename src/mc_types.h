#pragma once

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include "mc_packet.h"

template <size_t N> struct uint_of_size;
template <> struct uint_of_size<1> { using type = uint8_t; };
template <> struct uint_of_size<2> { using type = uint16_t; };
template <> struct uint_of_size<4> { using type = uint32_t; };
template <> struct uint_of_size<8> { using type = uint64_t; };

template <typename T>
concept WireScalar = std::is_arithmetic_v<T> && requires { typename uint_of_size<sizeof(T)>::type; };

template <WireScalar T>
constexpr std::array<uint8_t, sizeof(T)> to_be(T v) {
    auto u = std::bit_cast<typename uint_of_size<sizeof(T)>::type>(v);
    if constexpr (std::endian::native == std::endian::little) u = std::byteswap(u);
    return std::bit_cast<std::array<uint8_t, sizeof(T)>>(u);
}

template <WireScalar T>
constexpr T from_be(std::span<const uint8_t, sizeof(T)> bytes) {
    std::array<uint8_t, sizeof(T)> a{};
    for (size_t i = 0; i < sizeof(T); i++) a[i] = bytes[i];
    auto u = std::bit_cast<typename uint_of_size<sizeof(T)>::type>(a);
    if constexpr (std::endian::native == std::endian::little) u = std::byteswap(u);
    return std::bit_cast<T>(u);
}

struct VarInt {
    std::array<uint8_t, 5> buf{};
    size_t size = 0;
    constexpr std::span<const uint8_t> bytes() const { return {buf.data(), size}; }
};

constexpr VarInt encode_varint(int32_t value) {
    VarInt v;
    auto u = static_cast<uint32_t>(value);
    do {
        uint8_t b = u & 0x7F;
        u >>= 7;
        if (u) b |= 0x80;
        v.buf[v.size++] = b;
    } while (u);
    return v;
}

// Returns bytes consumed, or -1 if the buffer ends mid-varint or it runs past 5 bytes
constexpr int decode_varint(std::span<const uint8_t> buf, int32_t& out) {
    uint32_t v = 0;
    for (size_t i = 0; i < buf.size() && i < 5; i++) {
        v |= static_cast<uint32_t>(buf[i] & 0x7F) << (7 * i);
        if ((buf[i] & 0x80) == 0) {
            out = static_cast<int32_t>(v);
            return static_cast<int>(i + 1);
        }
    }
    return -1;
}

constexpr int64_t encode_position(int x, int y, int z) {
    return static_cast<int64_t>((static_cast<uint64_t>(x & 0x3FFFFFF) << 38) |
                                (static_cast<uint64_t>(z & 0x3FFFFFF) << 12) |
                                static_cast<uint64_t>(y & 0xFFF));
}

struct Uuid {
    uint64_t hi = 0, lo = 0;
    friend constexpr bool operator==(const Uuid&, const Uuid&) = default;
};

int mc_read_varint_sock(int sock, int32_t& out_value);

template <WireScalar T>
void pkt_write(PacketBuf& b, T v) { b.append(to_be(v)); }

inline void pkt_write_byte(PacketBuf& b, uint8_t v)   { pkt_write(b, v); }
inline void pkt_write_bool(PacketBuf& b, bool v)      { pkt_write<uint8_t>(b, v ? 1 : 0); }
inline void pkt_write_u16(PacketBuf& b, uint16_t v)   { pkt_write(b, v); }
inline void pkt_write_i16(PacketBuf& b, int16_t v)    { pkt_write(b, v); }
inline void pkt_write_i32(PacketBuf& b, int32_t v)    { pkt_write(b, v); }
inline void pkt_write_i64(PacketBuf& b, int64_t v)    { pkt_write(b, v); }
inline void pkt_write_f32(PacketBuf& b, float v)      { pkt_write(b, v); }
inline void pkt_write_f64(PacketBuf& b, double v)     { pkt_write(b, v); }
inline void pkt_write_varint(PacketBuf& b, int32_t v) { b.append(encode_varint(v).bytes()); }
inline void pkt_write_position(PacketBuf& b, int x, int y, int z) { pkt_write_i64(b, encode_position(x, y, z)); }
inline void pkt_write_uuid(PacketBuf& b, Uuid u) {
    pkt_write(b, u.hi);
    pkt_write(b, u.lo);
}
void pkt_write_string(PacketBuf& b, std::string_view s);

template <typename E>
concept PacketId = std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, int32_t>;

template <PacketId E>
void pkt_begin(PacketBuf& b, E id) {
    b.reset();
    pkt_write_varint(b, std::to_underlying(id));
}

// Reads never run past the end: on a short packet they set b.err and return
// zero, so callers check b.err once after parsing.
template <WireScalar T>
T pkt_read(PacketBuf& b) {
    if (b.err || b.unread().size() < sizeof(T)) {
        b.err = true;
        b.pos = b.len;
        return T{};
    }
    T v = from_be<T>(b.unread().first<sizeof(T)>());
    b.pos += sizeof(T);
    return v;
}

inline uint8_t  pkt_read_byte(PacketBuf& b) { return pkt_read<uint8_t>(b); }
inline uint16_t pkt_read_u16(PacketBuf& b)  { return pkt_read<uint16_t>(b); }
inline int64_t  pkt_read_i64(PacketBuf& b)  { return pkt_read<int64_t>(b); }
inline float    pkt_read_f32(PacketBuf& b)  { return pkt_read<float>(b); }
inline double   pkt_read_f64(PacketBuf& b)  { return pkt_read<double>(b); }
inline Uuid pkt_read_uuid(PacketBuf& b) {
    Uuid u;
    u.hi = pkt_read<uint64_t>(b);
    u.lo = pkt_read<uint64_t>(b);
    return u;
}
int32_t pkt_read_varint(PacketBuf& b);
// The view points into b, so it's only valid until b is next reused
std::string_view pkt_read_string(PacketBuf& b, size_t max_bytes);
