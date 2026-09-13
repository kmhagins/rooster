#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Station mode, register Wi-Fi event handlers,
 *        connect to the provided network credentials, and enable power saving.
 * 
 * @param ssid Wi-Fi network SSID
 * @param pass Wi-Fi network password
 */
void start_station_mode(const char *ssid, const char *pass);

/**
 * @brief Initialize SoftAP mode ("Coop_Hub_Setup"), launch the DNS hijacker task,
 *        and start the captive portal HTTP server.
 */
void start_provisioning_mode(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H