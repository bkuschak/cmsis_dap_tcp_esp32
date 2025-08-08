idf_component_register(SRCS "DAP.c" "DAP_vendor.c" "JTAG_DP.c" "SWO.c" "SW_DP.c" "UART.c" "cmsis_dap_tcp.c" "main.c"
                       PRIV_REQUIRES spi_flash
                       REQUIRES driver
                       REQUIRES lwip
                       REQUIRES esp_event
                       REQUIRES esp_timer
                       REQUIRES esp_wifi
                       REQUIRES nvs_flash
                       INCLUDE_DIRS "")
