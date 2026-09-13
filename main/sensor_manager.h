#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHT40_I2C_PORT      I2C_NUM_0
#define SHT40_I2C_SDA_PIN   GPIO_NUM_5
#define SHT40_I2C_SCL_PIN   GPIO_NUM_6
#define SHT40_I2C_ADDR      0x44

#define PIR_GPIO_PIN        GPIO_NUM_1

/**
 * @brief Initialize I2C_NUM_0, PIR GPIO interrupt, semaphores, and launch background tasks.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t sensor_manager_init(void);

/**
 * @brief Thread-safe getter for the latest SHT40 temperature and humidity.
 * 
 * @param[out] temp Pointer to store temperature in Celsius.
 * @param[out] hum  Pointer to store relative humidity in %.
 * @return true if valid sensor data is available, false otherwise.
 */
bool sensor_manager_get_sht40(float *temp, float *hum);

/**
 * @brief Acquire the lock and retrieve the pointer to the latest PSRAM JPEG snapshot.
 *        MUST call sensor_manager_unlock_snapshot() after transmission.
 * 
 * @param[out] out_buf Pointer to PSRAM buffer.
 * @param[out] out_len Pointer to JPEG size in bytes.
 * @return esp_err_t ESP_OK if a valid snapshot is ready, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t sensor_manager_lock_snapshot(const uint8_t **out_buf, size_t *out_len);

/**
 * @brief Release lock on the snapshot buffer.
 */
void sensor_manager_unlock_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_MANAGER_H