#ifndef CMSIS_DAP_TCP_H
#define CMSIS_DAP_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

// CMSIS-DAP requests are variable length. With CMSIS-DAP over USB, the
// transfer sizes are preserved by the USB stack. However, TCP/IP is stream
// oriented so we perform our own packetization to preserve the boundaries
// between each request. This short header is prepended to each CMSIS-DAP
// request and response before being sent over the socket. Little endian format
// is used for multibyte values.
// TODO h_u16_to_le() le_to_h_u16() for conversion.
struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;     // "DAP\0"
    uint16_t length;        // Not including header length.
    uint8_t packet_type;
    uint8_t reserved;       // Reserved for future use.
};

// If and when the protocol changes in the future, the SIGNATURE should be
// changed as well.
#define DAP_PKT_HDR_SIGNATURE   0x00504144
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02

#define CMSIS_DAP_TCP_PORT      4441    // Listen on this port.
#define CMSIS_DAP_PACKET_SIZE   1024    // Max payload size not including
                                        // header.

//#define CMSIS_DAP_TCP_SW_VERSION         0x0100      // 8 bit major, 8 bit minor
//#define CMSIS_DAP_TCP_PROTOCOL_VERSION   0x01        // Client side must match this.

//#define DEBUG_PRINTING

#ifdef DEBUG_PRINTING
#define LOG_DEBUG(...) \
{ \
    fprintf(stderr, "DEBUG: "); \
    fprintf(stderr, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#define LOG_DEBUG_IO(...) \
{ \
    fprintf(stderr, "DEBUG_IO: "); \
    fprintf(stderr, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#define LOG_ERROR(...) \
{ \
    fprintf(stderr, "ERROR: "); \
    fprintf(stderr, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#define LOG_INFO(...) \
{ \
    fprintf(stderr, "INFO: "); \
    fprintf(stderr, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}
#else
#define LOG_DEBUG(...) { }
#define LOG_DEBUG_IO(...) { }
#define LOG_ERROR(...) { }
#define LOG_INFO(...) { }
#endif

#if 0
/* general failures
 * error codes < 100
 */
#define ERROR_OK                        (0)
#define ERROR_NO_CONFIG_FILE            (-2)
#define ERROR_BUF_TOO_SMALL             (-3)
/* see "Error:" log entry for meaningful message to the user. The caller should
 * make no assumptions about what went wrong and try to handle the problem.
 */
#define ERROR_FAIL                      (-4)
#define ERROR_WAIT                      (-5)
#define ERROR_TIMEOUT_REACHED           (-6)
#define ERROR_NOT_IMPLEMENTED           (-7)
#endif

#if 0
// Initialize the library and provide callback functions.
// swd_* functions manipulate the SWD pins.  They should return 0 on success or
// <0 on error, except for swdio_read() which should return the pin state.
int remote_swd_server_init(uint32_t serial_num, uint16_t hw_version, uint16_t port_number,
        // Called once to initialize the pins.
        int (*swdio_swclk_init)(void),
        // Switch SWDIO to input mode.
        int (*swdio_input)(void),
        // Switch SWDIO to output mode.
        int (*swdio_output)(void),
        // Set SWDIO pin to 'bit'.
        int (*swdio_write)(bool bit),
        // Read state of SWDIO.
        bool (*swdio_read)(void),
        // Generate an active-high pulse on SWCLK.
        int (*swclk_send_pulse)(void),
        // Set the state of the SRST (NRST) pin.
        int (*set_srst)(bool val, bool open_drain));
#endif

// Initialize the server on the given TCP port number.
int cmsis_dap_tcp_init(int port_number);

// Handle client connections, receive and process any pending CMSIS-DAP
// requests, and send responses. Returns zero on success or nothing to do, and
// <0 on error.
int cmsis_dap_tcp_process(void);

#ifdef __cplusplus
}
#endif

#endif  // CMSIS_DAP_TCP_H
