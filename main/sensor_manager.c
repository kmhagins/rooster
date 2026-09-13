#include "sensor_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "sensor_mgr";

/* SHT40 Command: Measure High Repeatability */
#define SHT40_CMD_MEASURE_HIGH_PREC 0xFD
#define SHT40_READ_INTERVAL_MS      300000 // 5 minutes
#define PIR_COOLDOWN_US             5000000 // 5 seconds in microseconds

typedef struct {
    float temperature;
    float humidity;
    bool valid;
} sht40_readings_t;

static sht40_readings_t s_sht40_data = {0};
static SemaphoreHandle_t s_sht40_mutex = NULL;

static SemaphoreHandle_t s_pir_sem = NULL;
static SemaphoreHandle_t s_snapshot_mutex = NULL;
static uint8_t *s_latest_snapshot = NULL;
static size_t s_latest_snapshot_len = 0;

/* -------------------------------------------------------------------------
 * PIR ISR Handler
 * ------------------------------------------------------------------------- */
static void IRAM_ATTR pir_isr_handler(void *arg)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_pir_sem, &high_task_wakeup);
    if (high_task_wakeup) {
        portYIELD_FROM_ISR();
    }
}

/* -------------------------------------------------------------------------
 * PIR Task: Processes motion interrupts, enforces cooldown, allocates in PSRAM
 * ------------------------------------------------------------------------- */
static void pir_task(void *pvParameters)
{
    int64_t last_capture_time_us = 0;

    while (1) {
        if (xSemaphoreTake(s_pir_sem, portMAX_DELAY) == pdTRUE) {
            int64_t now_us = esp_timer_get_time();

            // Enforce 5-second cooldown
            if (last_capture_time_us != 0 && (now_us - last_capture_time_us) < PIR_COOLDOWN_US) {
                continue;
            }

            ESP_LOGI(TAG, "PIR Motion Triggered! Capturing frame...");
            last_capture_time_us = now_us;

            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                ESP_LOGE(TAG, "Camera capture failed on PIR event");
                continue;
            }

            // Allocate buffer strictly in Octal PSRAM
            uint8_t *psram_buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
            if (psram_buf != NULL) {
                memcpy(psram_buf, fb->buf, fb->len);

                xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
                // Free previous buffer to avoid memory leaks
                if (s_latest_snapshot != NULL) {
                    free(s_latest_snapshot);
                }
                s_latest_snapshot = psram_buf;
                s_latest_snapshot_len = fb->len;
                xSemaphoreGive(s_snapshot_mutex);

                ESP_LOGI(TAG, "Saved snapshot to PSRAM (%u bytes)", (unsigned int)fb->len);
            } else {
                ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM", (unsigned int)fb->len);
            }

            // Return frame buffer back to camera driver immediately
            esp_camera_fb_return(fb);
        }
    }
}

/* -------------------------------------------------------------------------
 * SHT40 Task: I2C Measurement every 5 minutes
 * ------------------------------------------------------------------------- */
static esp_err_t sht40_read(float *temp, float *hum)
{
    uint8_t cmd = SHT40_CMD_MEASURE_HIGH_PREC;
    esp_err_t ret = i2c_master_write_to_device(SHT40_I2C_PORT, SHT40_I2C_ADDR, &cmd, 1, pdMS_TO_TICKS(50));
    if (ret != ESP_OK) {
        return ret;
    }

    // High repeatability measurement takes ~8.2ms
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6];
    ret = i2c_master_read_from_device(SHT40_I2C_PORT, SHT40_I2C_ADDR, data, 6, pdMS_TO_TICKS(50));
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t t_ticks = (data[0] << 8) | data[1];
    uint16_t rh_ticks = (data[3] << 8) | data[4];

    *temp = -45.0f + 175.0f * ((float)t_ticks / 65535.0f);
    *hum = -6.0f + 125.0f * ((float)rh_ticks / 65535.0f);

    if (*hum < 0.0f) *hum = 0.0f;
    if (*hum > 100.0f) *hum = 100.0f;

    return ESP_OK;
}

static void sht40_task(void *pvParameters)
{
    while (1) {
        float t = 0.0f, h = 0.0f;
        if (sht40_read(&t, &h) == ESP_OK) {
            xSemaphoreTake(s_sht40_mutex, portMAX_DELAY);
            s_sht40_data.temperature = t;
            s_sht40_data.humidity = h;
            s_sht40_data.valid = true;
            xSemaphoreGive(s_sht40_mutex);
            ESP_LOGI(TAG, "SHT40 Read: Temp = %.2f C, Hum = %.2f %%", t, h);
        } else {
            ESP_LOGW(TAG, "SHT40 read failed");
        }

        // Sleep for 5 minutes (allows CPU to enter Light Sleep via esp_pm)
        vTaskDelay(pdMS_TO_TICKS(SHT40_READ_INTERVAL_MS));
    }
}

/* -------------------------------------------------------------------------
 * Public APIs
 * ------------------------------------------------------------------------- */
esp_err_t sensor_manager_init(void)
{
    s_sht40_mutex = xSemaphoreCreateMutex();
    s_snapshot_mutex = xSemaphoreCreateMutex();
    s_pir_sem = xSemaphoreCreateBinary();

    if (!s_sht40_mutex || !s_snapshot_mutex || !s_pir_sem) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS sync primitives");
        return ESP_ERR_NO_MEM;
    }

    // 1. Initialize I2C_NUM_0 for SHT40
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SHT40_I2C_SDA_PIN,
        .scl_io_num = SHT40_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };
    ESP_ERROR_CHECK(i2c_param_config(SHT40_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(SHT40_I2C_PORT, conf.mode, 0, 0, 0));

    // 2. Configure PIR GPIO & ISR
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIR_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_install_isr_service(0);
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIR_GPIO_PIN, pir_isr_handler, NULL));

    // 3. Create FreeRTOS Tasks
    xTaskCreate(sht40_task, "sht40_task", 3072, NULL, 4, NULL);
    xTaskCreate(pir_task, "pir_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Sensor manager initialized successfully");
    return ESP_OK;
}

bool sensor_manager_get_sht40(float *temp, float *hum)
{
    if (!s_sht40_mutex) return false;

    bool valid;
    xSemaphoreTake(s_sht40_mutex, portMAX_DELAY);
    *temp = s_sht40_data.temperature;
    *hum = s_sht40_data.humidity;
    valid = s_sht40_data.valid;
    xSemaphoreGive(s_sht40_mutex);
    return valid;
}

esp_err_t sensor_manager_lock_snapshot(const uint8_t **out_buf, size_t *out_len)
{
    if (!s_snapshot_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    if (s_latest_snapshot == NULL || s_latest_snapshot_len == 0) {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *out_buf = s_latest_snapshot;
    *out_len = s_latest_snapshot_len;
    return ESP_OK;
}

void sensor_manager_unlock_snapshot(void)
{
    if (s_snapshot_mutex) {
        xSemaphoreGive(s_snapshot_mutex);
    }
}