#pragma once

#include "mc_packet.h"

void send_play_packets(int sock, PacketBuf& out);
void send_chunk(int sock, PacketBuf& out, PacketBuf& scratch, int cx, int cz);
void send_center_chunk(int sock, PacketBuf& out, int cx, int cz);
