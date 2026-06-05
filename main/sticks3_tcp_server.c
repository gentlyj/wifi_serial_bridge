#include "sticks3_tcp_server.h"
#include "sticks3_uart_bridge.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>
#include <errno.h>

// mbedtls for WebSocket handshake SHA-1 / Base64
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"

static const char *TAG = "sticks3_ws";

#define BUF_SIZE    4096
#define FRAME_BUF_SIZE 4096
#define HANDSHAKE_BUF_SIZE 1024

static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

StreamBufferHandle_t tcp_to_uart_buf = NULL;
StreamBufferHandle_t uart_to_tcp_buf = NULL;

static TaskHandle_t s_server_task = NULL;
static int s_client_sock = -1;
static bool s_running = false;
static bool s_client_connected = false;

static uint8_t s_frame_buf[FRAME_BUF_SIZE];
static size_t  s_frame_len = 0;

static bool compute_accept_key(const char *client_key, char *out, size_t out_size) {
    char concat[256];
    int len = snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);

    unsigned char hash[20];
    mbedtls_sha1((const unsigned char *)concat, len, hash);

    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, out_size, &olen, hash, 20);
    return olen > 0;
}

static bool websocket_accept(int client) {
    char *buf = malloc(HANDSHAKE_BUF_SIZE);
    if (!buf) return false;

    int total = 0;
    bool headers_done = false;

    while (total < HANDSHAKE_BUF_SIZE - 1) {
        int n = recv(client, buf + total, HANDSHAKE_BUF_SIZE - 1 - total, 0);
        if (n <= 0) { free(buf); return false; }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) { headers_done = true; break; }
    }

    if (!headers_done) { free(buf); return false; }

    char *key_hdr = strstr(buf, "Sec-WebSocket-Key:");
    if (!key_hdr) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        send(client, resp, strlen(resp), 0);
        free(buf);
        return false;
    }

    key_hdr += 18;
    while (*key_hdr == ' ') key_hdr++;
    char key[64] = {0};
    int ki = 0;
    while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && ki < 63) {
        key[ki++] = *key_hdr++;
    }
    key[ki] = '\0';

    char accept_key[64];
    if (!compute_accept_key(key, accept_key, sizeof(accept_key))) {
        free(buf);
        return false;
    }

    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept_key);
    send(client, resp, rlen, 0);
    free(buf);

    ESP_LOGI(TAG, "WebSocket handshake OK");
    return true;
}

static void send_close_frame(int client) {
    uint8_t close_frame[2] = {0x88, 0x00};
    send(client, close_frame, 2, 0);
}

static void send_pong_frame(int client, const uint8_t *payload, int payload_len) {
    uint8_t hdr[2] = {0x8A, (uint8_t)payload_len};
    send(client, hdr, 2, 0);
    if (payload_len > 0) send(client, payload, payload_len, 0);
}

// Returns false if a close frame was received.
static bool process_frames(void) {
    while (s_frame_len >= 2) {
        uint8_t *p = s_frame_buf;
        uint8_t opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t payload_len = p[1] & 0x7F;
        int hdr_size = 2;

        if (payload_len == 126) {
            if (s_frame_len < 4) return true;
            payload_len = ((uint64_t)p[2] << 8) | p[3];
            hdr_size = 4;
        } else if (payload_len == 127) {
            if (s_frame_len < 10) return true;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | p[2 + i];
            hdr_size = 10;
        }

        if (masked) hdr_size += 4;
        int frame_size = hdr_size + (int)payload_len;

        if (payload_len > 32768) {
            ESP_LOGE(TAG, "Frame too large (%llu), resetting", payload_len);
            s_frame_len = 0;
            return true;
        }
        if (s_frame_len < (size_t)frame_size) return true;

        // Unmask payload
        if (masked) {
            uint8_t *mask = p + hdr_size - 4;
            uint8_t *data = p + hdr_size;
            for (int i = 0; i < (int)payload_len; i++)
                data[i] ^= mask[i & 3];
        }

        if (opcode == 0x08) {
            ESP_LOGI(TAG, "WS close frame received");
            send_close_frame(s_client_sock);
            s_frame_len = 0;
            return false;
        }
        if (opcode == 0x09) {
            send_pong_frame(s_client_sock, p + hdr_size, (int)payload_len);
            goto consume_frame;
        }
        if (opcode == 0x0A) {
            goto consume_frame;
        }

        // Data frame → UART
        if (payload_len > 0 && tcp_to_uart_buf) {
            xStreamBufferSend(tcp_to_uart_buf, p + hdr_size, (size_t)payload_len,
                              pdMS_TO_TICKS(100));
        }

consume_frame:
        {
            size_t remain = s_frame_len - frame_size;
            if (remain > 0) memmove(p, p + frame_size, remain);
            s_frame_len = remain;
        }
    }
    return true;
}

static void ws_task(void *arg) {
    int client = (int)(intptr_t)arg;
    s_frame_len = 0;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Clear stale data from previous connection
    xStreamBufferReset(tcp_to_uart_buf);
    xStreamBufferReset(uart_to_tcp_buf);

    s_client_connected = true;
    ESP_LOGI(TAG, "ws_task: client connected, buffers cleared");

    uint8_t uart_buf[512];

    while (s_running && s_client_sock >= 0) {
        if (s_frame_len < FRAME_BUF_SIZE) {
            int n = recv(client, s_frame_buf + s_frame_len,
                         FRAME_BUF_SIZE - s_frame_len, 0);
            if (n > 0) {
                s_frame_len += n;
            } else if (n == 0) {
                break;
            }
        }
        if (!process_frames()) break;

        // UART → WebSocket
        size_t len = xStreamBufferReceive(uart_to_tcp_buf, uart_buf,
                                          sizeof(uart_buf), 0);
        if (len > 0) {
            uint8_t hdr[10];
            int hdr_len;
            if (len < 126) {
                hdr[0] = 0x82;
                hdr[1] = (uint8_t)len;
                hdr_len = 2;
            } else {
                hdr[0] = 0x82;
                hdr[1] = 126;
                hdr[2] = (len >> 8) & 0xFF;
                hdr[3] = len & 0xFF;
                hdr_len = 4;
            }
            if (send(client, hdr, hdr_len, 0) <= 0) break;
            int sent = 0;
            while (sent < (int)len) {
                int n = send(client, uart_buf + sent, len - sent, 0);
                if (n <= 0) break;
                sent += n;
            }
        }
    }

    ESP_LOGI(TAG, "ws_task: client disconnected");
    close(client);
    s_client_sock = -1;
    s_client_connected = false;
    s_frame_len = 0;
    vTaskDelete(NULL);
}

static void server_task(void *arg) {
    uint16_t port = (uint16_t)(intptr_t)arg;
    ESP_LOGI(TAG, "server_task: started on port %d", port);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Socket create failed: errno=%d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno=%d", errno);
        close(listen_sock);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno=%d", errno);
        close(listen_sock);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listening on port %d", port);

    while (s_running) {
        ESP_LOGI(TAG, "Waiting for WebSocket client...");

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            if (s_running) ESP_LOGE(TAG, "Accept failed: errno=%d", errno);
            continue;
        }

        ESP_LOGI(TAG, "TCP from %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        if (!websocket_accept(client)) {
            ESP_LOGW(TAG, "WebSocket handshake failed");
            close(client);
            continue;
        }

        s_client_sock = client;
        xTaskCreate(ws_task, "ws_task", 8192, (void *)(intptr_t)client, 6, NULL);

        while (s_running && s_client_sock >= 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(TAG, "Client disconnected, ready for next");
    }

    close(listen_sock);
    ESP_LOGI(TAG, "server_task: exiting");
    s_server_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sticks3_tcp_server_start(uint16_t port) {
    if (s_running) return ESP_OK;

    // Create stream buffers once, shared with uart_bridge
    tcp_to_uart_buf = xStreamBufferCreate(BUF_SIZE, 1);
    uart_to_tcp_buf = xStreamBufferCreate(BUF_SIZE, 1);
    ESP_LOGI(TAG, "Stream buffers created");

    s_running = true;
    xTaskCreate(server_task, "ws_srv", 8192, (void *)(intptr_t)port, 5, &s_server_task);
    ESP_LOGI(TAG, "WebSocket server starting on port %d", port);
    return ESP_OK;
}

esp_err_t sticks3_tcp_server_stop(void) {
    if (!s_running) return ESP_OK;
    s_running = false;

    // Stop UART bridge first so tasks stop reading from buffers
    sticks3_uart_bridge_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_client_sock >= 0) {
        shutdown(s_client_sock, 0);
        close(s_client_sock);
        s_client_sock = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Now safe to delete buffers
    if (tcp_to_uart_buf) {
        vStreamBufferDelete(tcp_to_uart_buf);
        tcp_to_uart_buf = NULL;
    }
    if (uart_to_tcp_buf) {
        vStreamBufferDelete(uart_to_tcp_buf);
        uart_to_tcp_buf = NULL;
    }

    ESP_LOGI(TAG, "WebSocket server stopped");
    return ESP_OK;
}

bool sticks3_tcp_server_is_client_connected(void) {
    return s_client_connected;
}
