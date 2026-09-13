#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "camera_interface.h"
#include "wifi_manager.h"
#include "sensor_manager.h"

#define NVS_NAMESPACE       "wifi_config"
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASS        "wifi_pass"

static const char *TAG = "coop_main";

/* -------------------------------------------------------------------------
 * Power Management Initialization
 * ------------------------------------------------------------------------- */
static void init_power_management(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 40,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Power management enabled (Dynamic freq scaling & Light Sleep)");
#else
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE not enabled in sdkconfig; Power management inactive.");
#endif
}

/* -------------------------------------------------------------------------
 * Application Entry Point
 * ------------------------------------------------------------------------- */
void app_main(void)
{
    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Camera hardware (allocates framebuffers in PSRAM)
    ESP_LOGI(TAG, "Initializing Camera...");
    initialize_camera();

    // 3. Initialize SHT40, PIR interrupt, and background tasks
    ESP_ERROR_CHECK(sensor_manager_init());

    // 4. Initialize Power Management (Dynamic frequency scaling + Light Sleep)
    init_power_management();

    // 5. Check for stored Wi-Fi credentials
    nvs_handle_t nvs_h;
    char stored_ssid[33] = {0};
    char stored_pass[65] = {0};
    size_t ssid_len = sizeof(stored_ssid);
    size_t pass_len = sizeof(stored_pass);
    bool has_creds = false;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_h) == ESP_OK) {
        esp_err_t err_s = nvs_get_str(nvs_h, NVS_KEY_SSID, stored_ssid, &ssid_len);
        esp_err_t err_p = nvs_get_str(nvs_h, NVS_KEY_PASS, stored_pass, &pass_len);
        if (err_s == ESP_OK && err_p == ESP_OK && strlen(stored_ssid) > 0) {
            has_creds = true;
        }
        nvs_close(nvs_h);
    }

    // 6. Branch execution to Station Mode or SoftAP Provisioning Mode
    if (has_creds) {
        ESP_LOGI(TAG, "Found stored credentials for SSID: %s. Connecting...", stored_ssid);
        start_station_mode(stored_ssid, stored_pass);
    } else {
        ESP_LOGW(TAG, "No Wi-Fi credentials found. Launching Captive Portal...");
        start_provisioning_mode();
    }
}