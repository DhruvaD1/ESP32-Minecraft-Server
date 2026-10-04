#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "config.h"
#include "mc_types.h"
#include "mc_packet.h"
#include "mc_registry.h"
#include "mc_play.h"
#include "mc_players.h"
#include "mc_world.h"
#include "mc_protocol.h"

static constexpr const char* TAG = "mc_server";

enum class ConnState { HANDSHAKE, STATUS, LOGIN, CONFIG };

static volatile bool wifi_connected = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Connected on IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void wifi_init()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config{};
    static_assert(sizeof(WIFI_SSID) <= sizeof(wifi_config.sta.ssid), "WiFi SSID is too long");
    static_assert(sizeof(WIFI_PASSWORD) <= sizeof(wifi_config.sta.password), "WiFi password is too long");
    std::memcpy(wifi_config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID));
    std::memcpy(wifi_config.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done, connecting to \"%s\"", WIFI_SSID);
}

static std::atomic<int> g_connections{0};

static void send_status_response(int sock, PacketBuf& out) {
    char json[256];
    int len = snprintf(json, sizeof(json),
        "{\"version\":{\"name\":\"%s\",\"protocol\":%d},"
        "\"players\":{\"max\":%d,\"online\":%d},"
        "\"description\":{\"text\":\"ESP32-S3 Minecraft Server\"}}",
        MC_VERSION_NAME, MC_PROTOCOL_VERSION, MC_MAX_PLAYERS, players_online());

    pkt_begin(out, StatusOut::Response);
    pkt_write_string(out, {json, static_cast<size_t>(len)});
    out.send_packet(sock);
}

static void send_pong(int sock, PacketBuf& out, int64_t payload) {
    pkt_begin(out, StatusOut::Pong);
    pkt_write_i64(out, payload);
    out.send_packet(sock);
}

static void send_login_disconnect(int sock, PacketBuf& out, std::string_view reason) {
    char json[128];
    int len = snprintf(json, sizeof(json), "{\"text\":\"%.*s\"}", static_cast<int>(reason.size()), reason.data());
    pkt_begin(out, LoginOut::Disconnect);
    pkt_write_string(out, {json, std::min<size_t>(len, sizeof(json) - 1)});
    out.send_packet(sock);
}

static bool valid_username(std::string_view name) {
    return !name.empty() && name.size() <= 16 && std::ranges::all_of(name, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

static void update_view(int sock, PacketBuf& out, ChunkScratch& w,
                        int old_cx, int old_cz, int new_cx, int new_cz) {
    constexpr int vd = MC_VIEW_DISTANCE;
    auto outside = [](int cx, int cz, int center_x, int center_z) {
        return std::abs(cx - center_x) > vd || std::abs(cz - center_z) > vd;
    };
    send_center_chunk(sock, out, new_cx, new_cz);
    for (int cx = new_cx - vd; cx <= new_cx + vd; cx++)
        for (int cz = new_cz - vd; cz <= new_cz + vd; cz++)
            if (outside(cx, cz, old_cx, old_cz)) send_chunk(sock, out, w, cx, cz);
    for (int cx = old_cx - vd; cx <= old_cx + vd; cx++)
        for (int cz = old_cz - vd; cz <= old_cz + vd; cz++)
            if (outside(cx, cz, new_cx, new_cz)) send_unload_chunk(sock, out, cx, cz);
}

static void play_loop(int sock, int slot, PacketBuf& in, PacketBuf& out, ChunkScratch& w) {
    auto spawn = world_spawn();
    int center_cx = spawn.x >> 4, center_cz = spawn.z >> 4;
    TickType_t last_ka = xTaskGetTickCount();
    TickType_t last_rx = last_ka;

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        timeval tv{.tv_sec = 0, .tv_usec = 20000};
        int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) break;

        if (ret > 0) {
            if (!in.recv_packet(sock)) break;
            last_rx = xTaskGetTickCount();
            auto id = static_cast<PlayIn>(pkt_read_varint(in));
            std::optional<Vec3d> pos;
            std::optional<Rotation> rot;

            if (id == PlayIn::MovePos || id == PlayIn::MovePosRot) {
                double x = pkt_read_f64(in), y = pkt_read_f64(in), z = pkt_read_f64(in);
                pos = Vec3d{x, y, z};
            }
            if (id == PlayIn::MovePosRot || id == PlayIn::MoveRot) {
                float yaw = pkt_read_f32(in), pitch = pkt_read_f32(in);
                rot = Rotation{yaw, pitch};
            }

            if (pos || rot) {
                uint8_t flags = pkt_read_byte(in);
                if (!in.err) players_move(slot, pos, rot, flags & 1);
            } else if (id == PlayIn::ChatMessage) {
                auto msg = pkt_read_string(in, 256 * 4);
                if (!in.err && !msg.empty()) players_chat(slot, msg);
            } else if (id == PlayIn::SwingArm) {
                int32_t hand = pkt_read_varint(in);
                if (!in.err) players_swing(slot, hand);
            }

            if (pos && !in.err && std::isfinite(pos->x) && std::isfinite(pos->z) &&
                std::fabs(pos->x) < 3.0e7 && std::fabs(pos->z) < 3.0e7) {
                int new_cx = static_cast<int>(std::floor(pos->x)) >> 4;
                int new_cz = static_cast<int>(std::floor(pos->z)) >> 4;
                if (new_cx != center_cx || new_cz != center_cz) {
                    update_view(sock, out, w, center_cx, center_cz, new_cx, new_cz);
                    center_cx = new_cx;
                    center_cz = new_cz;
                }
            }
        }

        if (!players_flush(slot, sock)) break;

        TickType_t now = xTaskGetTickCount();
        if ((now - last_ka) * portTICK_PERIOD_MS >= 10000) {
            pkt_begin(out, PlayOut::KeepAlive);
            pkt_write_i64(out, static_cast<int64_t>(now));
            if (!out.send_packet(sock)) break;
            last_ka = now;
        }
        if ((now - last_rx) * portTICK_PERIOD_MS >= 30000) {
            ESP_LOGW(TAG, "Client timed out");
            break;
        }
    }
}

static std::optional<int> handle_login(int sock, PacketBuf& in, PacketBuf& out) {
    ConnState state = ConnState::HANDSHAKE;
    int32_t proto_ver = 0;
    std::optional<int> slot;

    auto give_up = [&]() -> std::optional<int> {
        if (slot) players_leave(*slot);
        return std::nullopt;
    };

    while (in.recv_packet(sock)) {
        int32_t packet_id = pkt_read_varint(in);
        if (in.err) break;

        switch (state) {
        case ConnState::HANDSHAKE: {
            if (packet_id != std::to_underlying(HandshakeIn::Handshake)) break;
            proto_ver = pkt_read_varint(in);
            auto server_addr = pkt_read_string(in, 1024);
            uint16_t server_port = pkt_read_u16(in);
            int32_t next_state = pkt_read_varint(in);
            if (in.err) return give_up();

            ESP_LOGI(TAG, "Handshake: proto=%d addr=%.*s port=%d next=%d",
                     static_cast<int>(proto_ver), static_cast<int>(server_addr.size()), server_addr.data(),
                     server_port, static_cast<int>(next_state));

            if (next_state == 1) state = ConnState::STATUS;
            else if (next_state == 2) state = ConnState::LOGIN;
            else return give_up();
            break;
        }

        case ConnState::STATUS:
            if (packet_id == std::to_underlying(StatusIn::Request)) {
                ESP_LOGI(TAG, "Status request -> sending response");
                send_status_response(sock, out);
            } else if (packet_id == std::to_underlying(StatusIn::Ping)) {
                int64_t payload = pkt_read_i64(in);
                if (!in.err) send_pong(sock, out, payload);
                return give_up();
            }
            break;

        case ConnState::LOGIN:
            if (packet_id == std::to_underlying(LoginIn::Start) && !slot) {
                auto username = pkt_read_string(in, 64);
                Uuid uuid = pkt_read_uuid(in);
                if (in.err) return give_up();

                ESP_LOGI(TAG, "Login Start: user=%.*s", static_cast<int>(username.size()), username.data());

                if (proto_ver != MC_PROTOCOL_VERSION) {
                    send_login_disconnect(sock, out, "This server runs Minecraft " MC_VERSION_NAME);
                    return give_up();
                }
                if (!valid_username(username)) {
                    send_login_disconnect(sock, out, "Invalid username");
                    return give_up();
                }
                auto joined = players_join(username, uuid);
                if (!joined) {
                    send_login_disconnect(sock, out, joined.error());
                    return give_up();
                }
                slot = *joined;

                pkt_begin(out, LoginOut::Success);
                pkt_write_uuid(out, uuid);
                pkt_write_string(out, username);
                pkt_write_varint(out, 0);
                out.send_packet(sock);

                ESP_LOGI(TAG, "Sent Login Success, waiting for ack");
            } else if (packet_id == std::to_underlying(LoginIn::Acknowledged) && slot) {
                ESP_LOGI(TAG, "Login Acknowledged -> Configuration state");
                state = ConnState::CONFIG;
                send_config_packets(sock, out);
            }
            break;

        case ConnState::CONFIG:
            if (packet_id == std::to_underlying(ConfigIn::FinishAcknowledged)) {
                ESP_LOGI(TAG, "Client acknowledged config -> Play state");
                return slot;
            }
            break;
        }
    }
    return give_up();
}

static void handle_client(int sock) {
    PacketBuf in(1024), out(1024);
    if (!in.allocated() || !out.allocated()) return;

    auto slot = handle_login(sock, in, out);
    if (!slot) return;

    if (ChunkScratch w; w.ok()) {
        send_play_packets(sock, out, w, players_eid(*slot));
        players_enter_play(*slot);
        play_loop(sock, *slot, in, out, w);
    } else {
        ESP_LOGE(TAG, "Out of memory for chunk buffers");
    }
    players_leave(*slot);
}

static void client_task(void* arg) {
    int sock = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    handle_client(sock);

    // Let the client hang up first. Closing with unread data sends a RST that can
    // wipe out a kick message, and closing first leaves us in TIME_WAIT, where lwIP
    // ignores new connections from that client port (kept short via TCP_MSL).
    timeval drain_to{.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &drain_to, sizeof(drain_to));
    std::array<uint8_t, 256> junk;
    for (int i = 0; i < 64 && recv(sock, junk.data(), junk.size(), 0) > 0; i++) {}
    close(sock);
    g_connections--;
    ESP_LOGI(TAG, "Connection closed (stack headroom %u bytes)",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

static void tcp_server_task(void* pvParameters)
{
    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MC_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    if (listen(listen_sock, 4) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Server listening on port %d", MC_PORT);

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_sock < 0) {
            ESP_LOGE(TAG, "Accept failed errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));

        // A couple of spare connections so server list pings still work when full
        if (g_connections >= MC_MAX_PLAYERS + 2) {
            ESP_LOGW(TAG, "Too many connections, dropping %s", addr_str);
            close(client_sock);
            continue;
        }
        ESP_LOGI(TAG, "New connection from %s:%d (internal heap free %u, lowest %u)",
                 addr_str, ntohs(client_addr.sin_port),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));

        int nodelay = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        timeval rcv_to{.tv_sec = 30, .tv_usec = 0}, snd_to{.tv_sec = 10, .tv_usec = 0};
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

        g_connections++;
        if (xTaskCreatePinnedToCore(client_task, "mc_client", 12288,
                                    reinterpret_cast<void*>(static_cast<intptr_t>(client_sock)),
                                    5, nullptr, 1) != pdPASS) {
            ESP_LOGE(TAG, "No memory for a client task");
            g_connections--;
            close(client_sock);
        }
    }
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "ESP32-S3 Minecraft Server");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM: %u bytes", static_cast<unsigned>(psram_size));
    } else {
        ESP_LOGW(TAG, "No PSRAM detected");
    }

    ESP_LOGI(TAG, "Free heap: %u bytes", static_cast<unsigned>(esp_get_free_heap_size()));

    wifi_init();

    // esp_random is only truly random once the radio is up, so seed after wifi starts
    uint32_t seed = MC_WORLD_SEED ? static_cast<uint32_t>(MC_WORLD_SEED) : esp_random();
    world_init(seed);
    auto [sx, sy, sz] = world_spawn();
    ESP_LOGI(TAG, "World seed %u, spawn at %d %d %d", static_cast<unsigned>(seed), sx, sy, sz);

    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 6144, nullptr, 5, nullptr, 0);
}
