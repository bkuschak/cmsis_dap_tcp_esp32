#ifndef COMMANDS_H
#define COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_wifi_types_generic.h"

// Sets up the console (REPL over whichever transport CONFIG_ESP_CONSOLE_*
// selects) and registers all commands. Starts the REPL running.
void commands_init(void);

// Returns true and fills in the stored WiFi credentials if valid ones were
// previously saved via the 'wifi' console command, false otherwise (caller
// should fall back to its own CONFIG defaults).
bool commands_get_stored_wifi_credentials(const char **ssid,
        const char **password, wifi_auth_mode_t *auth_mode);

#ifdef __cplusplus
}
#endif

#endif // COMMANDS_H
