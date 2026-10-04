#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include "mc_packet.h"

// Network NBT root compound (no name, just tag type 0x0A)
void nbt_begin(PacketBuf& b);
void nbt_end(PacketBuf& b);

void nbt_compound(PacketBuf& b, std::string_view name);
void nbt_byte(PacketBuf& b, std::string_view name, int8_t val);
void nbt_int(PacketBuf& b, std::string_view name, int32_t val);
void nbt_long(PacketBuf& b, std::string_view name, int64_t val);
void nbt_float(PacketBuf& b, std::string_view name, float val);
void nbt_double(PacketBuf& b, std::string_view name, double val);
void nbt_string(PacketBuf& b, std::string_view name, std::string_view val);
void nbt_string_list(PacketBuf& b, std::string_view name, std::span<const std::string_view> items);
void nbt_long_array(PacketBuf& b, std::string_view name, std::span<const int64_t> vals);
