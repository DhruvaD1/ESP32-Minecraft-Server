#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <atomic>
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

static constexpr const char* TAG = "mc_server";

enum class ConnState { HANDSHAKE, STATUS, LOGIN, CONFIG, PLAY };

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
    snprintf(json, sizeof(json),
        "{\"version\":{\"name\":\"%s\",\"protocol\":%d},"
        "\"players\":{\"max\":%d,\"online\":%d},"
        "\"description\":{\"text\":\"ESP32-S3 Minecraft Server\"}}",
        MC_VERSION_NAME, MC_PROTOCOL_VERSION, MC_MAX_PLAYERS, players_online());

    out.reset();
    pkt_write_varint(out, 0x00);
    pkt_write_string(out, json);
    out.send_packet(sock);
}

static void send_pong(int sock, PacketBuf& out, int64_t payload) {
    out.reset();
    pkt_write_varint(out, 0x01);
    pkt_write_i64(out, payload);
    out.send_packet(sock);
}

static void send_login_disconnect(int sock, PacketBuf& out, const char* reason) {
    char json[128];
    snprintf(json, sizeof(json), "{\"text\":\"%s\"}", reason);
    out.reset();
    pkt_write_varint(out, 0x00);
    pkt_write_string(out, json);
    out.send_packet(sock);
}

static bool valid_username(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n > 16) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_') return false;
    return true;
}

static void update_view(int sock, PacketBuf& out, ChunkScratch& w,
                        int old_cx, int old_cz, int new_cx, int new_cz) {
    const int vd = MC_VIEW_DISTANCE;
    send_center_chunk(sock, out, new_cx, new_cz);
    for (int cx = new_cx - vd; cx <= new_cx + vd; cx++)
        for (int cz = new_cz - vd; cz <= new_cz + vd; cz++)
            if (abs(cx - old_cx) > vd || abs(cz - old_cz) > vd)
                send_chunk(sock, out, w, cx, cz);
    for (int cx = old_cx - vd; cx <= old_cx + vd; cx++)
        for (int cz = old_cz - vd; cz <= old_cz + vd; cz++)
            if (abs(cx - new_cx) > vd || abs(cz - new_cz) > vd)
                send_unload_chunk(sock, out, cx, cz);
}

static void play_loop(int sock, int slot, PacketBuf& in, PacketBuf& out, ChunkScratch& w) {
    int sx, sy, sz;
    world_spawn(sx, sy, sz);
    int center_cx = sx >> 4, center_cz = sz >> 4;
    TickType_t last_ka = xTaskGetTickCount();
    TickType_t last_rx = last_ka;
    char msg[1024];

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {0, 20000};
        int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) break;

        if (ret > 0) {
            if (!in.recv_packet(sock)) break;
            last_rx = xTaskGetTickCount();
            int32_t pkt_id = pkt_read_varint(in);
            double pos[3];
            float rot[2];
            bool has_pos = false, has_rot = false;

            if (pkt_id == 0x1C || pkt_id == 0x1D) {
                for (double& v : pos) v = pkt_read_f64(in);
                has_pos = true;
            }
            if (pkt_id == 0x1D || pkt_id == 0x1E) {
                rot[0] = pkt_read_f32(in);
                rot[1] = pkt_read_f32(in);
                has_rot = true;
            }
            if (has_pos || has_rot) {
                uint8_t flags = pkt_read_byte(in);
                if (!in.err)
                    players_move(slot, has_pos ? pos : nullptr, has_rot ? rot : nullptr, flags & 1);
            } else if (pkt_id == 0x07) {
                pkt_read_string(in, msg, sizeof(msg));
                if (!in.err && msg[0]) players_chat(slot, msg);
            } else if (pkt_id == 0x3A) {
                int32_t hand = pkt_read_varint(in);
                if (!in.err) players_swing(slot, hand);
            }

            if (has_pos && !in.err && std::isfinite(pos[0]) && std::isfinite(pos[2]) &&
                fabs(pos[0]) < 3.0e7 && fabs(pos[2]) < 3.0e7) {
                int new_cx = static_cast<int>(floor(pos[0])) >> 4;
                int new_cz = static_cast<int>(floor(pos[2])) >> 4;
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
            out.reset();
            pkt_write_varint(out, 0x27);
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

static void handle_client(int sock) {
    PacketBuf in, out;
    if (!in.init() || !out.init()) {
        in.free();
        out.free();
        return;
    }

    ConnState state = ConnState::HANDSHAKE;
    int32_t proto_ver = 0;
    int slot = -1;
    bool play = false;

    while (!play && in.recv_packet(sock)) {
        int32_t packet_id = pkt_read_varint(in);
        if (in.err) break;

        if (state == ConnState::HANDSHAKE && packet_id == 0x00) {
            proto_ver = pkt_read_varint(in);
            char server_addr[256];
            pkt_read_string(in, server_addr, sizeof(server_addr));
            uint16_t server_port = pkt_read_u16(in);
            int32_t next_state = pkt_read_varint(in);
            if (in.err) break;

            ESP_LOGI(TAG, "Handshake: proto=%d addr=%s port=%d next=%d",
                     proto_ver, server_addr, server_port, next_state);

            if (next_state == 1) state = ConnState::STATUS;
            else if (next_state == 2) state = ConnState::LOGIN;
            else break;
            continue;
        }

        if (state == ConnState::STATUS) {
            if (packet_id == 0x00) {
                ESP_LOGI(TAG, "Status request -> sending response");
                send_status_response(sock, out);
            } else if (packet_id == 0x01) {
                int64_t payload = pkt_read_i64(in);
                if (!in.err) send_pong(sock, out, payload);
                break;
            }
            continue;
        }

        if (state == ConnState::LOGIN) {
            if (packet_id == 0x00 && slot < 0) {
                char username[32];
                pkt_read_string(in, username, sizeof(username));
                uint64_t uuid_hi, uuid_lo;
                pkt_read_uuid(in, uuid_hi, uuid_lo);
                if (in.err) break;

                ESP_LOGI(TAG, "Login Start: user=%s", username);

                if (proto_ver != MC_PROTOCOL_VERSION) {
                    send_login_disconnect(sock, out, "This server runs Minecraft " MC_VERSION_NAME);
                    break;
                }
                if (!valid_username(username)) {
                    send_login_disconnect(sock, out, "Invalid username");
                    break;
                }
                const char* reason = "";
                slot = players_join(username, uuid_hi, uuid_lo, reason);
                if (slot < 0) {
                    send_login_disconnect(sock, out, reason);
                    break;
                }

                out.reset();
                pkt_write_varint(out, 0x02);
                pkt_write_uuid(out, uuid_hi, uuid_lo);
                pkt_write_string(out, username);
                pkt_write_varint(out, 0);
                out.send_packet(sock);

                ESP_LOGI(TAG, "Sent Login Success, waiting for ack");
            } else if (packet_id == 0x03 && slot >= 0) {
                ESP_LOGI(TAG, "Login Acknowledged -> Configuration state");
                state = ConnState::CONFIG;
                send_config_packets(sock, out);
            }
            continue;
        }

        if (state == ConnState::CONFIG && packet_id == 0x03) {
            ESP_LOGI(TAG, "Client acknowledged config -> Play state");
            state = ConnState::PLAY;
            play = true;
        }
    }

    if (play) {
        ChunkScratch w{};
        if (w.init()) {
            send_play_packets(sock, out, w, players_eid(slot));
            players_enter_play(slot);
            play_loop(sock, slot, in, out, w);
        } else {
            ESP_LOGE(TAG, "Out of memory for chunk buffers");
        }
        w.free();
    }

    if (slot >= 0) players_leave(slot);
    in.free();
    out.free();
}

static void client_task(void* arg) {
    int sock = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    handle_client(sock);

    // Let the client hang up first. Closing with unread data sends a RST that can
    // wipe out a kick message, and closing first leaves us in TIME_WAIT, where lwIP
    // ignores new connections from that client port (kept short via TCP_MSL).
    struct timeval drain_to = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &drain_to, sizeof(drain_to));
    uint8_t junk[256];
    for (int i = 0; i < 64 && recv(sock, junk, sizeof(junk), 0) > 0; i++) {}
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
        struct timeval rcv_to = {30, 0}, snd_to = {10, 0};
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

    players_init();
    wifi_init();

    // esp_random is only truly random once the radio is up, so seed after wifi starts
    uint32_t seed = MC_WORLD_SEED ? static_cast<uint32_t>(MC_WORLD_SEED) : esp_random();
    world_init(seed);
    int sx, sy, sz;
    world_spawn(sx, sy, sz);
    ESP_LOGI(TAG, "World seed %u, spawn at %d %d %d", static_cast<unsigned>(seed), sx, sy, sz);

    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 6144, nullptr, 5, nullptr, 0);
}
