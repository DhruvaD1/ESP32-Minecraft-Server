#pragma once

#include <cstdint>

void players_init();
// Claims a slot for a player who just sent Login Start. Returns -1 and sets
// reason when the server is full or the name is already online.
int  players_join(const char* name, uint64_t uuid_hi, uint64_t uuid_lo, const char*& reason);
int32_t players_eid(int slot);
void players_enter_play(int slot);
void players_leave(int slot);
void players_move(int slot, const double* pos, const float* rot, bool on_ground);
void players_chat(int slot, const char* msg);
void players_swing(int slot, int hand);
bool players_flush(int slot, int sock);
int  players_online();
