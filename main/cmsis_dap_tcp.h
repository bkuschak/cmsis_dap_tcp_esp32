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
struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;         // "DAP"
    uint16_t length;            // Not including header length.
    uint8_t packet_type;
    uint8_t reserved;           // Reserved for future use.
};

// If the protocol changes in the future, SIGNATURE should be changed as well.
#define DAP_PKT_HDR_SIGNATURE   0x00504144
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02

#define CMSIS_DAP_TCP_PORT      4441    // Listen on this port.
#define CMSIS_DAP_PACKET_SIZE   1024    // Max payload size not including
                                        // header.

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
