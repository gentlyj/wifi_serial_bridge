#include "sticks3_provision.h"
#include "sticks3_wifi.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sticks3_prov";

static TaskHandle_t s_http_task = NULL;
static TaskHandle_t s_dns_task = NULL;
static bool s_active = false;
static esp_netif_t *s_ap_netif = NULL;

// Captive portal HTML
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>S3 WiFi Setup</title></head>"
    "<body style=\"font-family:sans-serif;text-align:center;padding:20px\">"
    "<h2>S3 WiFi Setup</h2>"
    "<form method=\"POST\" action=\"/connect\">"
    "<p>SSID:<br><input name=\"ssid\" type=\"text\" required style=\"width:200px;padding:5px\"></p>"
    "<p>Password:<br><input name=\"pass\" type=\"password\" style=\"width:200px;padding:5px\"></p>"
    "<p><button type=\"submit\" style=\"padding:10px 30px;font-size:16px\">Connect</button></p>"
    "</form></body></html>";

static void send_response(int client, const char *status, const char *body) {
    char header[256];
    int body_len = strlen(body);
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, body_len);
    send(client, header, hdr_len, 0);
    send(client, body, body_len, 0);
}

static void url_decode(char *dst, const char *src, size_t dst_size) {
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool parse_form_field(const char *body, const char *name, char *out, size_t out_size) {
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(body, key);
    if (!p) return false;
    p += strlen(key);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == 0) return false;
    char raw[256];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(out, raw, out_size);
    return true;
}

static void handle_client(int client) {
    char buf[1024];
    int len = recv(client, buf, sizeof(buf) - 1, 0);
    if (len <= 0) {
        close(client);
        return;
    }
    buf[len] = '\0';

    // Check if it's a POST to /connect
    if (strncmp(buf, "POST /connect", 13) == 0) {
        // Find body (after \r\n\r\n)
        char *body = strstr(buf, "\r\n\r\n");
        if (!body) {
            send_response(client, "400 Bad Request", "Missing body");
            close(client);
            return;
        }
        body += 4;

        char ssid[64] = {0};
        char pass[128] = {0};
        parse_form_field(body, "ssid", ssid, sizeof(ssid));
        parse_form_field(body, "pass", pass, sizeof(pass));

        if (strlen(ssid) == 0) {
            send_response(client, "400 Bad Request", "Missing SSID");
            close(client);
            return;
        }

        ESP_LOGI(TAG, "Connect: ssid=[%s]", ssid);

        // Send "connecting" response first
        send_response(client, "200 OK",
            "<html><body style=\"text-align:center;padding:40px\">"
            "<h2>Connecting to WiFi...</h2>"
            "<p>Please wait 15 seconds, then reconnect to your WiFi.</p>"
            "</body></html>");
        close(client);

        // Now try to connect
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_set_mode(WIFI_MODE_STA);
        sticks3_wifi_connect(ssid, pass);

        EventGroupHandle_t eg = sticks3_wifi_get_event_group();
        EventBits_t bits = xEventGroupWaitBits(eg,
            BIT0 | BIT1, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));

        if (bits & BIT0) {
            ESP_LOGI(TAG, "Provisioning success");
            s_active = false;
        } else {
            ESP_LOGW(TAG, "Provisioning failed");
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        }
        return;
    }

    // For any GET request (including captive portal detection), serve the portal page
    send_response(client, "200 OK", PORTAL_HTML);
    close(client);
}

static void http_server_task(void *arg) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "HTTP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "HTTP bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    listen(listen_sock, 2);
    ESP_LOGI(TAG, "HTTP server started on port 80");

    while (s_active) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            if (s_active) ESP_LOGE(TAG, "HTTP accept failed");
            continue;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client);
    }

    close(listen_sock);
    ESP_LOGI(TAG, "HTTP server stopped");
    s_http_task = NULL;
    vTaskDelete(NULL);
}

// DNS redirect task
static void dns_redirect_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS redirect task started");
    uint8_t buf[512];

    while (s_active) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;

        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;
        memcpy(resp, buf, len);

        resp[2] = 0x81; resp[3] = 0x80;
        resp[6] = 0x00; resp[7] = 0x01;

        int qname_end = 12;
        while (qname_end < len && buf[qname_end] != 0) qname_end += buf[qname_end] + 1;
        qname_end += 5;

        int resp_len = qname_end;
        resp[resp_len++] = 0xC0; resp[resp_len++] = 0x0C;
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x01;
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x01;
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x00;
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x3C;
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x04;
        resp[resp_len++] = 192; resp[resp_len++] = 168;
        resp[resp_len++] = 4;   resp[resp_len++] = 1;

        sendto(sock, resp, resp_len, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS redirect task stopped");
    vTaskDelete(NULL);
}

esp_err_t sticks3_provision_start(void) {
    if (s_active) return ESP_OK;

    ESP_LOGI(TAG, "Starting provisioning...");

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "S3-Setup",
            .ssid_len = 8,
            .password = "12345678",
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 1,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    s_active = true;
    xTaskCreate(http_server_task, "http_task", 4096, NULL, 5, &s_http_task);
    xTaskCreate(dns_redirect_task, "dns_task", 4096, NULL, 3, &s_dns_task);

    ESP_LOGI(TAG, "Provisioning started: AP=S3-Setup");
    return ESP_OK;
}

esp_err_t sticks3_provision_stop(void) {
    if (!s_active) return ESP_OK;

    s_active = false;
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_wifi_set_mode(WIFI_MODE_STA);

    ESP_LOGI(TAG, "Provisioning stopped");
    return ESP_OK;
}

bool sticks3_provision_is_active(void) {
    return s_active;
}
