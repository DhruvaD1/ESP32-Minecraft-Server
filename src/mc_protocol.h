#pragma once

#include <cstdint>

// Packet ids for protocol 769 (1.21.4)

enum class HandshakeIn : int32_t { Handshake = 0x00 };

enum class StatusIn : int32_t { Request = 0x00, Ping = 0x01 };
enum class StatusOut : int32_t { Response = 0x00, Pong = 0x01 };

enum class LoginIn : int32_t { Start = 0x00, Acknowledged = 0x03 };
enum class LoginOut : int32_t { Disconnect = 0x00, Success = 0x02 };

enum class ConfigIn : int32_t { FinishAcknowledged = 0x03 };
enum class ConfigOut : int32_t {
    FinishConfiguration = 0x03,
    RegistryData = 0x07,
    FeatureFlags = 0x0C,
    KnownPacks = 0x0E,
};

enum class PlayIn : int32_t {
    ChatMessage = 0x07,
    MovePos = 0x1C,
    MovePosRot = 0x1D,
    MoveRot = 0x1E,
    SwingArm = 0x3A,
};

enum class PlayOut : int32_t {
    SpawnEntity = 0x01,
    Animation = 0x03,
    SyncEntityPosition = 0x20,
    UnloadChunk = 0x22,
    GameEvent = 0x23,
    KeepAlive = 0x27,
    ChunkData = 0x28,
    Login = 0x2C,
    PlayerInfoRemove = 0x3F,
    PlayerInfoUpdate = 0x40,
    SyncPlayerPosition = 0x42,
    RemoveEntities = 0x47,
    HeadRotation = 0x4D,
    SetCenterChunk = 0x58,
    SpawnPosition = 0x5B,
    SystemChat = 0x73,
};
