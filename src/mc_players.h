#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include "mc_types.h"

struct Vec3d {
    double x, y, z;
};

struct Rotation {
    float yaw, pitch;
};

// Claims a slot for a player who just sent Login Start, or says why they can't join
std::expected<int, std::string_view> players_join(std::string_view name, Uuid uuid);
int32_t players_eid(int slot);
void players_enter_play(int slot);
void players_leave(int slot);
void players_move(int slot, std::optional<Vec3d> pos, std::optional<Rotation> rot, bool on_ground);
void players_chat(int slot, std::string_view msg);
void players_swing(int slot, int hand);
bool players_flush(int slot, int sock);
int  players_online();
