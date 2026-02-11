#pragma once

#include <cstdint>
#include <cstddef>

int mc_read_varint(const uint8_t* buf, size_t buf_len, int32_t& out_value);
int mc_write_varint(uint8_t* buf, int32_t value);
int mc_varint_size(int32_t value);
int mc_read_varint_sock(int sock, int32_t& out_value);

uint16_t mc_read_u16(const uint8_t* buf);
int16_t  mc_read_i16(const uint8_t* buf);
int32_t  mc_read_i32(const uint8_t* buf);
int64_t  mc_read_i64(const uint8_t* buf);
float    mc_read_f32(const uint8_t* buf);
double   mc_read_f64(const uint8_t* buf);

void mc_write_u16(uint8_t* buf, uint16_t val);
void mc_write_i16(uint8_t* buf, int16_t val);
void mc_write_i32(uint8_t* buf, int32_t val);
void mc_write_i64(uint8_t* buf, int64_t val);
void mc_write_f32(uint8_t* buf, float val);
void mc_write_f64(uint8_t* buf, double val);

int64_t mc_encode_position(int x, int y, int z);
void mc_decode_position(int64_t val, int& x, int& y, int& z);

struct PacketBuf;
void pkt_write_byte(PacketBuf& b, uint8_t val);
void pkt_write_bool(PacketBuf& b, bool val);
void pkt_write_u16(PacketBuf& b, uint16_t val);
void pkt_write_i16(PacketBuf& b, int16_t val);
void pkt_write_i32(PacketBuf& b, int32_t val);
void pkt_write_i64(PacketBuf& b, int64_t val);
void pkt_write_f32(PacketBuf& b, float val);
void pkt_write_f64(PacketBuf& b, double val);
void pkt_write_varint(PacketBuf& b, int32_t val);
void pkt_write_string(PacketBuf& b, const char* str);
void pkt_write_position(PacketBuf& b, int x, int y, int z);
void pkt_write_uuid(PacketBuf& b, uint64_t hi, uint64_t lo);

uint8_t  pkt_read_byte(PacketBuf& b);
bool     pkt_read_bool(PacketBuf& b);
uint16_t pkt_read_u16(PacketBuf& b);
int16_t  pkt_read_i16(PacketBuf& b);
int32_t  pkt_read_i32(PacketBuf& b);
int64_t  pkt_read_i64(PacketBuf& b);
float    pkt_read_f32(PacketBuf& b);
double   pkt_read_f64(PacketBuf& b);
int32_t  pkt_read_varint(PacketBuf& b);
size_t   pkt_read_string(PacketBuf& b, char* out, size_t max_len);
void     pkt_read_position(PacketBuf& b, int& x, int& y, int& z);
void     pkt_read_uuid(PacketBuf& b, uint64_t& hi, uint64_t& lo);
