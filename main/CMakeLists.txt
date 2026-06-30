set(COMPONENT_SRCS
    "DAP.c"
    "DAP_vendor.c"
    "JTAG_DP.c"
    "SWO.c"
    "SW_DP.c"
    "UART.c"
    "cmsis_dap_tcp.c"
    "main.c")

set(PRIV_REQUIRES "spi_flash" "esp_driver_gpio" "esp_driver_uart")

if(CONFIG_ESP_UART_BRIDGE_ENABLED)
    list(APPEND COMPONENT_SRCS "uart_bridge.c")
endif()

if(CONFIG_ESP_PRINT_CPU_USAGE)
    list(APPEND COMPONENT_SRCS "cpu_usage.c")
endif()

if(CONFIG_ESP_DAP_LED_RGB)
    list(APPEND COMPONENT_SRCS "ws2812_led.c")
    list(APPEND PRIV_REQUIRES "esp_driver_rmt" "espressif__led_strip")
endif()

idf_component_register(SRCS ${COMPONENT_SRCS}
                       PRIV_REQUIRES ${PRIV_REQUIRES}
                       REQUIRES lwip esp_event esp_timer esp_wifi nvs_flash console
                       INCLUDE_DIRS ".")
