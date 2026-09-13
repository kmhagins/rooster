#include "web_server.h"
#include "sensor_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE       "wifi_config"
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASS        "wifi_pass"

static const char *TAG = "web_server";

/* -------------------------------------------------------------------------
 * Utility: URL Decoding
 * ------------------------------------------------------------------------- */
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t i = 0;
    while (*src && i < dst_len - 1) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* -------------------------------------------------------------------------
 * Station Mode Handlers (Live Dashboard & Cached PSRAM Snapshot)
 * ------------------------------------------------------------------------- */
static esp_err_t sta_root_get_handler(httpd_req_t *req)
{
    float temp = 0.0f, hum = 0.0f;
    char temp_str[16] = "--";
    char hum_str[16] = "--";

    if (sensor_manager_get_sht40(&temp, &hum)) {
        snprintf(temp_str, sizeof(temp_str), "%.1f &deg;C", temp);
        snprintf(hum_str, sizeof(hum_str), "%.1f %%", hum);
    }

    char html[1536];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html>"
        "<head><meta charset=\"UTF-8\"><title>Smart Chicken Coop Hub</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;text-align:center;background:#1e1e1e;color:#f0f0f0;margin:0;padding:20px;}"
        ".metrics{display:flex;justify-content:center;gap:20px;margin:20px auto;max-width:500px;}"
        ".card{background:#2a2a2a;padding:15px 25px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.3);flex:1;}"
        ".val{font-size:24px;font-weight:bold;color:#4caf50;margin-top:5px;}"
        ".img-box{margin:20px auto;max-width:800px;border-radius:10px;overflow:hidden;box-shadow:0 4px 10px rgba(0,0,0,0.6);background:#000;}"
        "img{width:100%%;height:auto;display:block;}"
        "</style></head>"
        "<body>"
        "<h1>Coop Environmental Hub</h1>"
        "<div class=\"metrics\">"
        "  <div class=\"card\"><div>Temperature</div><div class=\"val\">%s</div></div>"
        "  <div class=\"card\"><div>Humidity</div><div class=\"val\">%s</div></div>"
        "</div>"
        "<h3>Latest Motion Capture</h3>"
        "<div class=\"img-box\"><img id=\"cam\" src=\"/image.jpg\" alt=\"Awaiting motion detection...\"></div>"
        "<script>"
        "  setInterval(() => {"
        "    document.getElementById('cam').src = '/image.jpg?t=' + new Date().getTime();"
        "  }, 5000);"
        "</script>"
        "</body></html>",
        temp_str, hum_str
    );

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t sta_image_get_handler(httpd_req_t *req)
{
    const uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;

    esp_err_t err = sensor_manager_lock_snapshot(&jpeg_buf, &jpeg_len);
    if (err != ESP_OK || jpeg_buf == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "No motion snapshot captured yet", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=motion.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    esp_err_t res = httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);

    sensor_manager_unlock_snapshot();
    return res;
}

httpd_handle_t start_sta_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.stack_size = 4096;

    static const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = sta_root_get_handler, .user_ctx = NULL
    };
    static const httpd_uri_t img_uri = {
        .uri = "/image.jpg", .method = HTTP_GET, .handler = sta_image_get_handler, .user_ctx = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &img_uri);
        ESP_LOGI(TAG, "STA Web server started on port 80");
        return server;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Captive Portal Handlers (SoftAP Provisioning)
 * ------------------------------------------------------------------------- */
static esp_err_t captive_root_get_handler(httpd_req_t *req)
{
    const char *html = 
        "<!DOCTYPE html><html>"
        "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Coop Hub Setup</title>"
        "<style>"
        "body{font-family:sans-serif;padding:20px;max-width:400px;margin:auto;background:#f4f4f4;}"
        ".card{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
        "input[type=text],input[type=password]{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;}"
        "input[type=submit]{width:100%;background:#2e7d32;color:white;padding:12px;border:none;border-radius:4px;cursor:pointer;font-size:16px;font-weight:bold;}"
        "</style></head>"
        "<body><div class=\"card\">"
        "<h2>Chicken Coop Wi-Fi Setup</h2>"
        "<form action=\"/configure\" method=\"POST\">"
        "<label>Network Name (SSID):</label><input type=\"text\" name=\"ssid\" required>"
        "<label>Password:</label><input type=\"password\" name=\"password\">"
        "<input type=\"submit\" value=\"Save & Connect\">"
        "</form></div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t captive_configure_post_handler(httpd_req_t *req)
{
    char post_buf[256];
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;

    if (total_len >= sizeof(post_buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too long");
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        received = httpd_req_recv(req, post_buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        cur_len += received;
    }
    post_buf[total_len] = '\0';

    char raw_ssid[64] = {0};
    char raw_pass[64] = {0};
    char decoded_ssid[64] = {0};
    char decoded_pass[64] = {0};

    if (httpd_query_key_value(post_buf, "ssid", raw_ssid, sizeof(raw_ssid)) == ESP_OK) {
        url_decode(decoded_ssid, raw_ssid, sizeof(decoded_ssid));
    }
    if (httpd_query_key_value(post_buf, "password", raw_pass, sizeof(raw_pass)) == ESP_OK) {
        url_decode(decoded_pass, raw_pass, sizeof(decoded_pass));
    }

    if (strlen(decoded_ssid) > 0) {
        nvs_handle_t nvs_h;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
        if (err == ESP_OK) {
            nvs_set_str(nvs_h, NVS_KEY_SSID, decoded_ssid);
            nvs_set_str(nvs_h, NVS_KEY_PASS, decoded_pass);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
            ESP_LOGI(TAG, "Wi-Fi credentials saved for SSID: %s", decoded_ssid);
        }

        const char *resp = "<!DOCTYPE html><html><body><h2>Credentials saved!</h2><p>Coop Hub is restarting to connect to your network...</p></body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
    }

    return ESP_OK;
}

httpd_handle_t start_captive_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    config.stack_size = 4096;

    static const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = captive_root_get_handler, .user_ctx = NULL
    };
    static const httpd_uri_t config_uri = {
        .uri = "/configure", .method = HTTP_POST, .handler = captive_configure_post_handler, .user_ctx = NULL
    };
    static const httpd_uri_t redir_gen204 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL
    };
    static const httpd_uri_t redir_gen204_alt = {
        .uri = "/gen_204", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL
    };
    static const httpd_uri_t redir_apple = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL
    };
    static const httpd_uri_t redir_ncsi = {
        .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL
    };
    static const httpd_uri_t redir_connecttest = {
        .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &config_uri);
        httpd_register_uri_handler(server, &redir_gen204);
        httpd_register_uri_handler(server, &redir_gen204_alt);
        httpd_register_uri_handler(server, &redir_apple);
        httpd_register_uri_handler(server, &redir_ncsi);
        httpd_register_uri_handler(server, &redir_connecttest);
        ESP_LOGI(TAG, "Captive portal HTTP server started");
        return server;
    }
    return NULL;
}

void stop_webserver(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}