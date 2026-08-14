set(COMPONENT_SRCS
    "DAP.c"
    "DAP_gpio_config.c"
    "DAP_vendor.c"
    "JTAG_DP.c"
    "SWO.c"
    "SW_DP.c"
    "UART.c"
    "cmsis_dap_tcp.c"
    "tls_transport.c")

# Include the app entrypoint only for the standalone firmware project, so
# other ESP-IDF projects can consume this directory as a component.
if(CMAKE_PROJECT_NAME STREQUAL "cmsis_dap_tcp_esp32")
    list(APPEND COMPONENT_SRCS "main.c")
endif()

set(PRIV_REQUIRES "spi_flash" "esp_driver_gpio" "esp_adc" "esp_app_format" "app_update")

# mbedtls required here. stripped out later if CONFIG_ESP_TLS_ENABLED=n
set(REQUIRES
    "lwip" "esp_driver_uart" "esp_event" "esp_timer" "esp_wifi" "mbedtls"
    "nvs_flash" "console")

if(CONFIG_ESP_UART_BRIDGE_1_ENABLED OR
   CONFIG_ESP_UART_BRIDGE_2_ENABLED OR
   CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    list(APPEND COMPONENT_SRCS "uart_bridge.c")
endif()

if(CONFIG_ESP_DAP_MANAGE_WIFI)
    list(APPEND COMPONENT_SRCS "wifi.c")
endif()

if(CONFIG_ESP_ADC_STREAM_ENABLED)
    list(APPEND COMPONENT_SRCS "adc_stream.c")
    list(APPEND PRIV_REQUIRES "esp_adc")
endif()

if(CONFIG_ESP_OTA_ENABLED)
    list(APPEND COMPONENT_SRCS "ota_tcp.c")
endif()

if(CONFIG_ESP_DAP_SERIAL_CONSOLE_ENABLED OR
   CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED)
    list(APPEND COMPONENT_SRCS "commands.c")
endif()

if(CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED)
    list(APPEND COMPONENT_SRCS "socket_console.c")
    list(APPEND PRIV_REQUIRES "espressif__esp_linenoise")
endif()

if(CONFIG_ESP_PRINT_CPU_USAGE)
    list(APPEND COMPONENT_SRCS "cpu_usage.c")
endif()

set(TLS_EMBED "")
if(CONFIG_ESP_TLS_ENABLED)
    set(TLS_EMBED "certs/servercert.pem" "certs/prvtkey.pem" "certs/cacert.pem")

    # Auto-provisions certs on first build. No-op if certs already exist.
    execute_process(
        COMMAND "${COMPONENT_DIR}/../host/gen_esp32_certs.sh"
        RESULT_VARIABLE GEN_CERTS_RESULT
    )
    if(NOT GEN_CERTS_RESULT EQUAL 0)
        message(FATAL_ERROR
            "gen_esp32_certs.sh failed (exit ${GEN_CERTS_RESULT}) -- see "
            "main/certs/README.md, or place your own certs in main/certs/.")
    endif()
endif()

if(CONFIG_ESP_DAP_1_LED_RGB OR
   CONFIG_ESP_DAP_2_LED_RGB OR
   CONFIG_ESP_DAP_3_LED_RGB)
    list(APPEND COMPONENT_SRCS "ws2812_led.c")
    list(APPEND PRIV_REQUIRES "esp_driver_rmt" "espressif__led_strip")
endif()

idf_component_register(SRCS ${COMPONENT_SRCS}
                       PRIV_REQUIRES ${PRIV_REQUIRES}
                       REQUIRES ${REQUIRES}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES ${TLS_EMBED})

