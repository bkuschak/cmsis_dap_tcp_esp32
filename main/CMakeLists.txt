set(COMPONENT_SRCS "DAP.c" "DAP_vendor.c" "JTAG_DP.c" "SWO.c" "SW_DP.c" "UART.c" "cmsis_dap_tcp.c" "main.c")

if(CONFIG_ESP_UART_BRIDGE_ENABLED)
    list(APPEND COMPONENT_SRCS "uart_bridge.c")
endif()

idf_component_register(SRCS ${COMPONENT_SRCS}
                       PRIV_REQUIRES spi_flash
                       REQUIRES driver
                       REQUIRES lwip
                       REQUIRES esp_event
                       REQUIRES esp_timer
                       REQUIRES esp_wifi
                       REQUIRES nvs_flash
                       INCLUDE_DIRS "")
