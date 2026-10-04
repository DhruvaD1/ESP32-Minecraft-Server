#pragma once

#include "mc_packet.h"
#include "mc_world.h"

void send_play_packets(int sock, PacketBuf& out, ChunkScratch& w, int32_t eid);
void send_chunk(int sock, PacketBuf& out, ChunkScratch& w, int cx, int cz);
void send_unload_chunk(int sock, PacketBuf& out, int cx, int cz);
void send_center_chunk(int sock, PacketBuf& out, int cx, int cz);
