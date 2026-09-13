#include "wifi_manager.h"
#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#define NVS_NAMESPACE       "wifi_config"
#define AP_SSID             "Coop_Hub_Setup"
#define AP_MAX_CONN         4
#define MAXIMUM_STA_RETRY   5

#define DNS_PORT            53
#define DNS_MAX_LEN         512

static const char *TAG = "wifi_manager";

static int s_retry_num = 0;
static httpd_handle_t s_server = NULL;

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* -------------------------------------------------------------------------
 * mDNS Initialization
 * ------------------------------------------------------------------------- */
static void init_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("rooster"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Smart Chicken Coop Hub"));
    ESP_ERROR_CHECK(mdns_service_add("CoopWebServer", "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS initialized: http://rooster.local");
}

/* -------------------------------------------------------------------------
 * DNS Hijacker Task (Captive Portal DNS Responder)
 * ------------------------------------------------------------------------- */
static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[DNS_MAX_LEN];
    uint8_t tx_buffer[DNS_MAX_LEN];

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Hijacker listening on port %d...", DNS_PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &addr_len);
        if (len < 12) {
            continue;
        }

        // Copy Query Header to Response
        memcpy(tx_buffer, rx_buffer, len);

        // Standard query response, No error (0x8180)
        tx_buffer[2] = 0x81;
        tx_buffer[3] = 0x80;

        // Set ANCOUNT = 1, NSCOUNT = 0, ARCOUNT = 0
        tx_buffer[6] = 0x00;
        tx_buffer[7] = 0x01;
        tx_buffer[8] = 0x00;
        tx_buffer[9] = 0x00;
        tx_buffer[10] = 0x00;
        tx_buffer[11] = 0x00;

        // Walk to the end of Question Section
        int idx = 12;
        while (idx < len && rx_buffer[idx] != 0) {
            idx += rx_buffer[idx] + 1;
        }
        idx += 5; // Skip NULL terminator (1 byte) + QTYPE (2 bytes) + QCLASS (2 bytes)

        if (idx > len || idx + 16 > DNS_MAX_LEN) {
            continue;
        }

        // Craft DNS Answer (A-Record pointing unconditionally to 192.168.4.1)
        tx_buffer[idx++] = 0xC0; // Name pointer
        tx_buffer[idx++] = 0x0C; // Points to offset 12 (Question Name)
        tx_buffer[idx++] = 0x00; // Type: A record (0x0001)
        tx_buffer[idx++] = 0x01;
        tx_buffer[idx++] = 0x00; // Class: IN (0x0001)
        tx_buffer[idx++] = 0x01;
        tx_buffer[idx++] = 0x00; // TTL: 60 seconds
        tx_buffer[idx++] = 0x00;
        tx_buffer[idx++] = 0x00;
        tx_buffer[idx++] = 0x3C;
        tx_buffer[idx++] = 0x00; // RDLENGTH: 4 bytes
        tx_buffer[idx++] = 0x04;
        tx_buffer[idx++] = 192;  // RDATA: 192.168.4.1
        tx_buffer[idx++] = 168;
        tx_buffer[idx++] = 4;
        tx_buffer[idx++] = 1;

        sendto(sock, tx_buffer, idx, 0, (struct sockaddr *)&source_addr, addr_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Station Mode Event Handler
 * ------------------------------------------------------------------------- */
static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_server) {
            stop_webserver(s_server);
            s_server = NULL;
        }
        if (s_retry_num < MAXIMUM_STA_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying AP connection...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        init_mdns();
        if (s_server == NULL) {
            s_server = start_sta_webserver();
        }
    }
}

/* -------------------------------------------------------------------------
 * Public Functions
 * ------------------------------------------------------------------------- */
void start_station_mode(const char *ssid, const char *pass)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.listen_interval = 3;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Enable modem power saving
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect with stored credentials. Erasing and restarting to provisioning mode...");
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_erase_all(nvs_h);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

void start_provisioning_mode(void)
{
    ESP_LOGI(TAG, "Starting SoftAP Provisioning Mode...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .password = "",
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP '%s' started. IP: 192.168.4.1", AP_SSID);

    // Start DNS Hijacker Task & Captive Portal Server
    xTaskCreate(dns_server_task, "dns_hijacker", 4096, NULL, 5, NULL);
    s_server = start_captive_webserver();
}