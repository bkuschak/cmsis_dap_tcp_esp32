#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

typedef enum {
    WIFI_OPERATING_MODE_STA = 0,
    WIFI_OPERATING_MODE_AP = 1,
} wifi_operating_mode_t;

// Initializes one WiFi interface, selected by NVS override or menuconfig.
// Station mode blocks until connected or out of retries.
// Returns 0 on success, -1 on failure.
int wifi_init(void);

// Tells wifi.c that a network-dependent TCP service (CMSIS-DAP-TCP) has
// started listening. From this point on, losing the AP connection reboots
// the device instead of just retrying -- the listening/connected sockets
// can't be recovered without reinitializing the whole network stack.
void wifi_mark_service_started(void);

// Reports current STA connectivity or SoftAP SSID. *rssi is only valid in
// Station mode when *connected is true.
void wifi_get_status(bool *connected, const char **ssid, int *rssi);

// Write new WiFi credentials to flash. Takes effect on next reboot.
esp_err_t wifi_save_config(const char *ssid, const char *password,
        wifi_auth_mode_t auth_mode);

// Erase credentials from flash. Subsequent boots fall back to CONFIG defaults.
esp_err_t wifi_clear_config(void);

// SoftAP settings are independent of Station credentials. Takes effect after
// reboot. Omitted command-line channel uses the menuconfig default.
esp_err_t wifi_save_ap_config(const char *ssid, const char *password,
        uint8_t channel);
esp_err_t wifi_clear_ap_config(void);

// Exactly one WiFi mode is active. Changes persist in NVS and take effect
// after reboot; clearing the override restores the menuconfig default.
wifi_operating_mode_t wifi_get_operating_mode(void);
wifi_operating_mode_t wifi_get_default_operating_mode(void);
esp_err_t wifi_save_operating_mode(wifi_operating_mode_t mode);
esp_err_t wifi_clear_operating_mode(void);

// Reports the active SoftAP channel (valid when AP mode is active).
uint8_t wifi_get_ap_channel(void);
// True if NVS overrides or menuconfig provide a valid WPA2 SoftAP setup.
bool wifi_ap_is_configured(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H
