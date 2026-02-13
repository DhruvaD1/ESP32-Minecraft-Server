#pragma once

#include "mc_packet.h"

// Network NBT root compound (no name, just tag type 0x0A)
void nbt_begin(PacketBuf& b);
void nbt_end(PacketBuf& b);

void nbt_compound(PacketBuf& b, const char* name);
void nbt_byte(PacketBuf& b, const char* name, int8_t val);
void nbt_int(PacketBuf& b, const char* name, int32_t val);
void nbt_long(PacketBuf& b, const char* name, int64_t val);
void nbt_float(PacketBuf& b, const char* name, float val);
void nbt_double(PacketBuf& b, const char* name, double val);
void nbt_string(PacketBuf& b, const char* name, const char* val);
void nbt_string_list(PacketBuf& b, const char* name, const char** items, int count);
void nbt_long_array(PacketBuf& b, const char* name, const int64_t* vals, int32_t count);
