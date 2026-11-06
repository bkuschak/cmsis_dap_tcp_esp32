/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi: Station or SoftAP (mutually exclusive), selected via menuconfig
 * with an optional NVS override. Credentials for both modes are stored
 * separately and can be updated using the console.
 *
 * Some parts of this code were adapted from:
 *     esp-idf/examples/wifi/getting_started/station
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/dhcp6.h"
#include "lwip/prot/dhcp6.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wifi.h"
#include "reboot.h"

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""

#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID

#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN

#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP

#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK

#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* Use an event group to signal two WiFi related events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Keep the Station namespace/keys intact for backward compatibility. */
#define NVS_NAMESPACE           "wifi_config"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "password"
#define NVS_KEY_AUTH_MODE       "auth_mode"
#define MAX_SSID_LEN            32
#define MAX_PASSWORD_LEN        64

#define AP_NVS_NAMESPACE        "softap_config"
#define AP_NVS_KEY_CHANNEL      "channel"
#define AP_SSID_LEN             33  // 32 bytes plus NUL
#define AP_PASSWORD_LEN         64  // 63 bytes plus NUL

#define MODE_NVS_NAMESPACE      "wifi_mode"
#define MODE_NVS_KEY            "mode"

static int wifi_retry_num;
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif;
static bool service_started;
static volatile bool wifi_connected;

static wifi_operating_mode_t operating_mode =
#ifdef CONFIG_ESP_DAP_WIFI_MODE_AP
    WIFI_OPERATING_MODE_AP;
#else
    WIFI_OPERATING_MODE_STA;
#endif

static const char* wifi_ssid = CONFIG_ESP_WIFI_SSID;
static const char* wifi_password = CONFIG_ESP_WIFI_PASSWORD;
static wifi_auth_mode_t wifi_auth_mode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;

static char stored_ssid[MAX_SSID_LEN] = {0};
static char stored_password[MAX_PASSWORD_LEN] = {0};
static wifi_auth_mode_t stored_auth_mode = WIFI_AUTH_WPA2_PSK;

static const char* ap_ssid = CONFIG_ESP_DAP_SOFTAP_SSID;
static const char* ap_password = CONFIG_ESP_DAP_SOFTAP_PASSWORD;
static uint8_t ap_channel = CONFIG_ESP_DAP_SOFTAP_CHANNEL;
static char stored_ap_ssid[AP_SSID_LEN] = {0};
static char stored_ap_password[AP_PASSWORD_LEN] = {0};

wifi_operating_mode_t wifi_get_default_operating_mode(void)
{
#ifdef CONFIG_ESP_DAP_WIFI_MODE_AP
    return WIFI_OPERATING_MODE_AP;
#else
    return WIFI_OPERATING_MODE_STA;
#endif
}

wifi_operating_mode_t wifi_get_operating_mode(void)
{
    return operating_mode;
}

wifi_operating_mode_t wifi_get_selected_operating_mode(void)
{
    wifi_operating_mode_t mode = wifi_get_default_operating_mode();
    nvs_handle_t handle;
    if (nvs_open(MODE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return mode;

    uint8_t saved_mode;
    if (nvs_get_u8(handle, MODE_NVS_KEY, &saved_mode) == ESP_OK &&
        saved_mode <= WIFI_OPERATING_MODE_AP) {
        mode = (wifi_operating_mode_t)saved_mode;
    }
    nvs_close(handle);
    return mode;
}

static void load_operating_mode(void)
{
    operating_mode = wifi_get_selected_operating_mode();
}

esp_err_t wifi_save_operating_mode(wifi_operating_mode_t mode)
{
    if (mode != WIFI_OPERATING_MODE_STA && mode != WIFI_OPERATING_MODE_AP)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MODE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(handle, MODE_NVS_KEY, (uint8_t)mode);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_clear_operating_mode(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MODE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, MODE_NVS_KEY);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    // No saved override is already equivalent to menuconfig default.
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

esp_err_t wifi_save_config(const char *ssid, const char *password,
        wifi_auth_mode_t auth_mode)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_AUTH_MODE, (uint8_t)auth_mode);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t load_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = MAX_SSID_LEN;
    size_t password_len = MAX_PASSWORD_LEN;
    uint8_t auth_mode;

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, stored_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, stored_password,
                &password_len);
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, NVS_KEY_AUTH_MODE, &auth_mode);
        if (err == ESP_OK) {
            stored_auth_mode = (wifi_auth_mode_t)auth_mode;
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t wifi_clear_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Erase the entire namespace to clear all WiFi credentials.
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

// SoftAP overrides use a dedicated namespace, preserving Station settings.
esp_err_t wifi_save_ap_config(const char *ssid, const char *password,
        uint8_t channel)
{
    if (ssid == NULL || password == NULL || strlen(ssid) == 0 ||
        strlen(ssid) > 32 || strlen(password) < 8 ||
        strlen(password) > 63 || channel < 1 || channel > 11)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK)
        err = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    if (err == ESP_OK)
        err = nvs_set_u8(handle, AP_NVS_KEY_CHANNEL, channel);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_clear_ap_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_ap_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AP_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;

    size_t ssid_len = sizeof(stored_ap_ssid);
    size_t password_len = sizeof(stored_ap_password);
    uint8_t channel;
    err = nvs_get_str(handle, NVS_KEY_SSID, stored_ap_ssid, &ssid_len);
    if (err == ESP_OK)
        err = nvs_get_str(handle, NVS_KEY_PASSWORD, stored_ap_password,
                &password_len);
    if (err == ESP_OK)
        err = nvs_get_u8(handle, AP_NVS_KEY_CHANNEL, &channel);
    if (err == ESP_OK && stored_ap_ssid[0] != '\0' &&
        strlen(stored_ap_ssid) <= 32 &&
        strlen(stored_ap_password) >= 8 &&
        strlen(stored_ap_password) <= 63 && channel >= 1 && channel <= 11) {
        ap_ssid = stored_ap_ssid;
        ap_password = stored_ap_password;
        ap_channel = channel;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_ARG;
    }
    nvs_close(handle);
    return err;
}

bool wifi_ap_defaults_valid(void)
{
    size_t ssid_len = strlen(CONFIG_ESP_DAP_SOFTAP_SSID);
    size_t password_len = strlen(CONFIG_ESP_DAP_SOFTAP_PASSWORD);
    return ssid_len >= 1 && ssid_len <= 32 &&
           password_len >= 8 && password_len <= 63 &&
           CONFIG_ESP_DAP_SOFTAP_CHANNEL >= 1 &&
           CONFIG_ESP_DAP_SOFTAP_CHANNEL <= 11;
}

bool wifi_ap_is_configured(void)
{
    // Validate saved settings without changing the running SoftAP config.
    nvs_handle_t handle;
    if (nvs_open(AP_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        char ssid[AP_SSID_LEN];
        char password[AP_PASSWORD_LEN];
        size_t ssid_len = sizeof(ssid);
        size_t password_len = sizeof(password);
        uint8_t channel = 0;
        esp_err_t err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
        if (err == ESP_OK)
            err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &password_len);
        if (err == ESP_OK)
            err = nvs_get_u8(handle, AP_NVS_KEY_CHANNEL, &channel);
        nvs_close(handle);
        if (err == ESP_OK && strlen(ssid) >= 1 && strlen(ssid) <= 32 &&
            strlen(password) >= 8 && strlen(password) <= 63 &&
            channel >= 1 && channel <= 11)
            return true;
    }

    return wifi_ap_defaults_valid();
}

uint8_t wifi_get_ap_channel(void)
{
    return ap_channel;
}

void wifi_mark_service_started(void)
{
    if (operating_mode == WIFI_OPERATING_MODE_STA)
        service_started = true;
}

void wifi_get_status(bool *connected, const char **ssid, int *rssi)
{
    if (operating_mode == WIFI_OPERATING_MODE_AP) {
        *connected = false;  // Not a Station connection.
        *ssid = ap_ssid;
        *rssi = 0;
        return;
    }

    *connected = wifi_connected;
    *ssid = wifi_ssid;
    *rssi = 0;
    if (wifi_connected)
        esp_wifi_sta_get_rssi(rssi);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            printf("Attempting to connect to WiFi SSID: '%s'\n", wifi_ssid);
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            int rssi = 0;
            esp_wifi_sta_get_rssi(&rssi);
            printf("Connected to WiFi SSID: '%s'. RSSI: %d dBm\n", wifi_ssid,
                    rssi);
            wifi_connected = true;
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            if (service_started) {
                /* If connection is lost after a network-dependent service
                 * has started, any connected sockets and the server
                 * socket have been lost. The server socket must be
                 * reinitialized. Just reboot to reinitialize everything.
                 */
                fprintf(stderr, "Lost connection to WiFi SSID: '%s'. Rebooting...\n",
                        wifi_ssid);
                reboot();       // Does not return.
            }
            if (wifi_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
                printf("Retrying connection to WiFi SSID: '%s'\n", wifi_ssid);
                esp_wifi_connect();
                wifi_retry_num++;
            }
            else {
                fprintf(stderr, "Failed to connect to WiFi SSID: '%s'.\n", wifi_ssid);
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("IP address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        wifi_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
#ifdef CONFIG_LWIP_IPV6
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
        esp_ip6_addr_type_t ipv6_type =
            esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        printf("IPv6 address (%s): " IPV6STR "\n",
                (ipv6_type == ESP_IP6_ADDR_IS_LINK_LOCAL) ? "link-local" :
                "global", IPV62STR(event->ip6_info.ip));
    }
#endif
}

/* The SoftAP interface uses the ESP-IDF default DHCP server and IPv4
 * address (typically 192.168.4.1/24).
 */
static int wifi_init_softap(void)
{
    if (load_ap_config() == ESP_OK)
        printf("Using SoftAP configuration from flash.\n");
    else
        printf("Using SoftAP configuration from menuconfig.\n");

    size_t ssid_len = strlen(ap_ssid);
    size_t password_len = strlen(ap_password);
    if (ssid_len == 0 || ssid_len > 32 ||
        password_len < 8 || password_len > 63 ||
        ap_channel < 1 || ap_channel > 11) {
        fprintf(stderr, "SoftAP: invalid SSID (1-32 bytes), WPA2 password (8-63 bytes), or channel (1-11).\n");
        return -1;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        fprintf(stderr, "SoftAP: failed to create network interface.\n");
        return -1;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = (uint8_t)ssid_len,
            .channel = ap_channel,
            .max_connection = CONFIG_ESP_DAP_SOFTAP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(ap_config.ap.ssid, ap_ssid, ssid_len);
    memcpy(ap_config.ap.password, ap_password, password_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));
    printf("SoftAP started: SSID='%s', channel=%u, IP=" IPSTR "\n",
           ap_ssid, (unsigned)ap_channel, IP2STR(&ip_info.ip));
    return 0;
}

int wifi_init(void)
{
    load_operating_mode();
    if (operating_mode == WIFI_OPERATING_MODE_AP)
        return wifi_init_softap();

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                &event_handler,
                NULL,
                &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &event_handler,
                NULL,
                &instance_got_ip));
#ifdef CONFIG_LWIP_IPV6
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_GOT_IP6,
                &event_handler,
                NULL,
                &instance_got_ip));
#endif

    // Try to load WiFi credentials saved via the 'wifi' console command,
    // and fall back to hardcoded CONFIG values.
    if (load_wifi_credentials() == ESP_OK && strlen(stored_ssid) > 0) {
        wifi_ssid = stored_ssid;
        wifi_password = stored_password;
        wifi_auth_mode = stored_auth_mode;
        printf("Using WiFi credentials from flash.\n");
    } else {
        printf("Using WiFi credentials from hardcoded CONFIG.\n");
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = wifi_auth_mode,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };

    // Copy SSID and password (need to handle const char* to uint8_t[]
    // conversion).
    strlcpy((char*)wifi_config.sta.ssid, wifi_ssid,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, wifi_password,
            sizeof(wifi_config.sta.password));
    if(wifi_auth_mode == WIFI_AUTH_OPEN) {
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait until either the connection is established (WIFI_CONNECTED_BIT)
     * or connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
     * The bits are set by event_handler() above.
     */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
#ifdef CONFIG_ESP_DAP_DISABLE_WIFI_POWER_SAVE
        // Disable power-save to improve WiFi performance.
        // https://github.com/espressif/arduino-esp32/issues/1484
        printf("Disabling WiFi power savings to improve performance.\n");
        esp_wifi_set_ps(WIFI_PS_NONE);
#endif
#ifdef CONFIG_LWIP_IPV6
        // Trigger IPv6 SLAAC auto-configuration, after IPv4 is up.
        esp_err_t err = esp_netif_create_ip6_linklocal(sta_netif);
        if (err != ESP_OK)
            perror("Failed to create IPv6 link local address");
#endif
        return 0;   // Success.
    }
    return -1;      // Failure.
}
