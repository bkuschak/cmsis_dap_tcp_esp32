set(COMPONENT_SRCS
    "DAP.c"
    "DAP_gpio_config.c"
    "DAP_vendor.c"
    "JTAG_DP.c"
    "SWO.c"
    "SW_DP.c"
    "UART.c"
    "cmsis_dap_tcp.c")

# Include the app entrypoint only for the standalone firmware project, so
# other ESP-IDF projects can consume this directory as a component.
if(CMAKE_PROJECT_NAME STREQUAL "cmsis_dap_tcp_esp32")
    list(APPEND COMPONENT_SRCS "main.c")
endif()

set(PRIV_REQUIRES "spi_flash" "esp_driver_gpio")

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
                       REQUIRES lwip
                       # Public (not PRIV_REQUIRES): uart_bridge.h exposes
                       # driver/uart.h's enum types in its public config
                       # struct, so dependents need this propagated too.
                       REQUIRES esp_driver_uart
                       REQUIRES esp_event
                       REQUIRES esp_timer
                       REQUIRES esp_wifi
                       REQUIRES nvs_flash
                       REQUIRES console
                       INCLUDE_DIRS ".")

# Allow git hash to be embedded in the DAP_ID_PRODUCT_FW_VER string.
include("${COMPONENT_DIR}/git_version.cmake")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    -DGIT_HASH="${GIT_HASH}"
    -DGIT_DATE="${GIT_DATE}"
    -DGIT_STATUS="${GIT_STATUS}"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/.git/index")

