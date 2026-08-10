#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

// Initializes WiFi and connects to the AP. Blocks until connected or out of
// retries. Returns 0 on success, -1 on failure.
int wifi_init(void);

// Tells wifi.c that a network-dependent TCP service (CMSIS-DAP-TCP) has
// started listening. From this point on, losing the AP connection reboots
// the device instead of just retrying -- the listening/connected sockets
// can't be recovered without reinitializing the whole network stack.
void wifi_mark_service_started(void);

// Reports current connection state. *rssi is only valid when *connected is
// true.
void wifi_get_status(bool *connected, const char **ssid, int *rssi);

// Write new WiFi credentials to flash. Takes effect on next reboot.
esp_err_t wifi_save_config(const char *ssid, const char *password,
        wifi_auth_mode_t auth_mode);

// Erase credentials from flash. Subsequent boots fall back to CONFIG defaults.
esp_err_t wifi_clear_config(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H
