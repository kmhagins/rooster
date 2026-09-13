#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP server for Station (STA) mode.
 *        Serves root live viewer (/) and JPEG stream endpoint (/image.jpg).
 * 
 * @return httpd_handle_t Handle to the started server, or NULL on error.
 */
httpd_handle_t start_sta_webserver(void);

/**
 * @brief Start the HTTP server for SoftAP Captive Portal provisioning mode.
 *        Serves configuration form (/) and captive redirect endpoints.
 * 
 * @return httpd_handle_t Handle to the started server, or NULL on error.
 */
httpd_handle_t start_captive_webserver(void);

/**
 * @brief Stop an active HTTP server instance.
 * 
 * @param server Handle to the HTTP server.
 */
void stop_webserver(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H