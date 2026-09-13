# Smart Chicken Coop Hub

A low-power, cloud-independent, solar-operated monitoring hub for backyard chicken coops. Built on the ESP32-S3, this hub prioritizes privacy, local-only control, and industry-standard embedded development practices.

## Hardware Architecture
* **Microcontroller:** Seeed Studio XIAO ESP32S3 Sense (8MB Flash, 8MB Octal PSRAM)
* **Camera:** OV3660 (Utilizes hardware JPEG compression)
* **Sensors (Planned):** SHT40 Temperature/Humidity (I2C), PIR Motion Sensor
* **Power:** Solar-charged battery

## Core Features
* **Native Captive Portal:** SoftAP provisioning with DNS hijacking. The hub broadcasts its own network (`Coop_Hub_Setup`) when no credentials are found, seamlessly popping up a configuration page on iOS and Android devices.
* **Persistent Configuration:** Wi-Fi credentials and system states are stored securely in Non-Volatile Storage (NVS).
* **Zero-Configuration Networking:** Resolves locally via mDNS to `http://coophub.local`.
* **Low-Power RTOS Design:** Implements `esp_pm` dynamic frequency scaling (DFS) and FreeRTOS tickless idle. The CPU clock-gates and drops into Light Sleep between Wi-Fi beacons and web requests to preserve solar battery life.

## Software Stack
* **Framework:** ESP-IDF v5.5.1
* **Language:** C
* **Build System:** CMake / Ninja

## Important Build Notes
### OV3660 Camera & I2C Drivers
Newer batches of the XIAO ESP32S3 Sense ship with the OV3660 sensor. Due to the strict timing requirements of ESP-IDF v5.x's new I2C hardware driver, the camera's proprietary SCCB protocol will fail to probe (`Error 0x106`). 

To resolve this, the project forces the use of the legacy I2C driver in `sdkconfig`:
```text
CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW=n
CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY=y

```

## Installation & Flashing

1. Ensure ESP-IDF v5.5.1 is exported to your terminal.
2. Clone the repository and navigate to the root directory.
3. Fetch the required Espressif registry dependencies:
```bash
idf.py add-dependency "espressif/mdns"
idf.py add-dependency "espressif/esp32-camera"

```


4. Build and flash the firmware:
```bash
idf.py build
idf.py flash monitor

```
