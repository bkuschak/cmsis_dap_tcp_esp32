// TCP server (should work on Linux, Mac, ESP32).

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "DAP.h"
#include "cmsis_dap_tcp.h"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define le_to_h_u16(a)  (a)
#define le_to_h_u32(a)  (a)
#define h_u16_to_le(a)  (a)
#define h_u32_to_le(a)  (a)
#else
#include <byteswap.h>
#define le_to_h_u16(a)  __bswap_16(a)
#define le_to_h_u32(a)  __bswap_32(a)
#define h_u16_to_le(a)  __bswap_16(a)
#define h_u32_to_le(a)  __bswap_32(a)
#endif

// Only a single client at a time may be connected.
static int client_sockfd;
static int server_sockfd;
static uint8_t request[CMSIS_DAP_PACKET_SIZE];
static uint8_t response[CMSIS_DAP_PACKET_SIZE];
static uint8_t packet_buf[CMSIS_DAP_PACKET_SIZE + sizeof(struct cmsis_dap_tcp_packet_hdr)];

static int socket_available(void)
{
    if(client_sockfd == 0)
        return 0;

    int nbytes;
    int ret = ioctl(client_sockfd, FIONREAD, &nbytes);
    if(ret < 0)
        return 0;
    return nbytes;
}

// Returns 1 if disconnected, or 0 if still connected, or <0 on error.
// https://stackoverflow.com/questions/5640144/c-how-to-use-select-to-see-if-a-socket-has-closed/5640173
static int socket_disconnected(void)
{
    char x;
    int r;

    // Non blocking peek to see if any data is available.
    // If client has disconnected, then recv() will return 0.
    while(true) {
        r = recv(client_sockfd, &x, 1, MSG_DONTWAIT|MSG_PEEK);
        if (r < 0) {
            switch (errno) {
                case EINTR:     continue;
                case EAGAIN:    break; /* empty rx queue */
                case ETIMEDOUT: break; /* recv timeout */
                case ENOTCONN:  break; /* not connected yet */
                default:        return -errno;
            }
        }
        break;
    }
    return r == 0;
}

// Start the server.
static int start_server(int port)
{
    int ret;
    struct sockaddr_in server_addr;

    server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_sockfd < 0) {
        fprintf(stderr, "cmsis_dap_tcp: failed to open server socket.\n");
        return -1;
    }

    int optval = 1;
    setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEPORT, &optval,
            sizeof(optval));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    ret = bind(server_sockfd, (void*)&server_addr, sizeof(server_addr));
    if(ret != 0) {
        fprintf(stderr, "cmsis_dap_tcp: failed to bind server socket.\n");
        return -1;
    }

    ret = fcntl(server_sockfd, F_SETFL, O_NONBLOCK);
    if(ret < 0) {
        fprintf(stderr, "cmsis_dap_tcp: failed to set nonblocking server socket.\n");
        return -1;
    }

    ret = listen(server_sockfd, 1);
    if(ret < 0) {
        fprintf(stderr, "cmsis_dap_tcp: failed to listen to server socket.\n");
        return -1;
    }

    fprintf(stderr, "cmsis_dap_tcp: listening on port %d.\n", port);
    return 0;
}

// Handle incoming connections on server socket. Non-blocking.
static int handle_server(void)
{
    // Handle a new client connecting.
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int ret = accept(server_sockfd, (void*)&client_addr, &len);
    if(ret < 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN)
            return 0; // No clients connecting.
        else
            return ret;
    }

    // We only support a single connected client. If a client is already
    // connected, drop new connections.
    if(client_sockfd != 0) {
        fprintf(stderr, "cmsis_dap_tcp: dropping new connection.\n");
        close(ret);
    }
    else {
        client_sockfd = ret;
        fprintf(stderr, "cmsis_dap_tcp: client connected %s:%d\n",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));

        // Use TCP keepalives to detect dead clients.
        int val = 1;
        setsockopt(client_sockfd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
#if defined(__linux__) || defined(ESP_PLATFORM)
        // Seconds between probes (Linux and ESP32)
        val = 1;
        setsockopt(client_sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &val,
                    sizeof(val));
        setsockopt(client_sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &val,
                    sizeof(val));
        // Number of probes to send before closing the connection.
        val = 5;
        setsockopt(client_sockfd, IPPROTO_TCP, TCP_KEEPCNT, &val,
                    sizeof(val));
#elif defined(__APPLE__)
        // Seconds between probes (MacOS). TCP_KEEPALIVE is like TCP_KEEPIDLE.
        val = 5;
        setsockopt(client_sockfd, IPPROTO_TCP, TCP_KEEPALIVE, &val,
                    sizeof(val));
#else
#warning "Platform not recognized! Cannot setup TCP keepalive."
#endif
        ret = fcntl(client_sockfd, F_SETFL, O_NONBLOCK);
        if(ret < 0) {
            fprintf(stderr, "cmsis_dap_tcp: failed to set nonblocking server socket.\n");
            return -1;
        }
    }
    return 0;
}

// Just check if the client has disconnected.
static int handle_client(void)
{
    if(client_sockfd == 0)
        return 0;

    if(socket_disconnected()) {
        fprintf(stderr, "cmsis_dap_tcp: client disconnected.\n");
        close(client_sockfd);
        client_sockfd = 0;
    }
    return 0;
}

// Read from socket. Return number of bytes, or <0 on error.
static int socket_read(void* data, int len)
{
    if(client_sockfd == 0)
        return -1;      // No client.

    return read(client_sockfd, data, len);
}

// Peek at the socket data, but don't remove the bytes from the socket buffer.
static int socket_peek(void* data, int len)
{
    if(client_sockfd == 0)
        return -1;      // No client.

    return recv(client_sockfd, data, len, MSG_PEEK);
}

// Read from socket. Return number of bytes, or <0 on error.
static int socket_write(void* data, int len)
{
    if(client_sockfd == 0)
        return -1;      // No client.

    return write(client_sockfd, data, len);
}

// ----------------------------------------------------------------------------

static int send_dap_response(uint8_t *buf, int len)
{
    struct cmsis_dap_tcp_packet_hdr *header = (void*)packet_buf;

    if(len > sizeof(packet_buf) - sizeof(*header)) {
        fprintf(stderr, "cmsis_dap_tcp: response too large for buffer!\n");
        return -1;
    }

    header->signature = h_u32_to_le(DAP_PKT_HDR_SIGNATURE);
    header->length = h_u16_to_le(len);
    header->packet_type = DAP_PKT_TYPE_RESPONSE;
    header->reserved = 0;

    uint8_t *payload = packet_buf + sizeof(*header);
    memcpy(payload, buf, len);

    len += sizeof(*header);
    int ret = socket_write(packet_buf, len);
    if(ret < 0)
        return ret;
    return ret == len ? 0 : -1;
}

// Read one complete DAP request packet from the socket if possible.
// Return number of bytes received on success or <0 on error.
// If a complete packet is not available, return 0.
static int recv_dap_request(uint8_t *buf, int len)
{
    struct cmsis_dap_tcp_packet_hdr header;

    if(socket_available() < sizeof(header))
        return 0;

    LOG_DEBUG_IO("Peeking at header");
    int ret = socket_peek(&header, sizeof(header));
    if(ret < 0)
        return ret;
    if(ret < sizeof(header))
        return -1;      /* Shouldn't happen */
    if(le_to_h_u32(header.signature) != DAP_PKT_HDR_SIGNATURE) {
        LOG_ERROR("Incorrect header signature 0x%08lx",
                le_to_h_u32(header.signature));
        socket_read(&header, sizeof(header));   // Discard.
        return -1;
    }
    if(header.packet_type != DAP_PKT_TYPE_REQUEST) {
        LOG_ERROR("Unrecognized packet type 0x%02hx", header.packet_type);
        socket_read(&header, sizeof(header));   // Discard.
        return -1;
    }
    if(socket_available() < sizeof(header) + le_to_h_u16(header.length))
        return 0;

    // A complete packet is available. Strip the header and return the data.
    if(len < le_to_h_u16(header.length)) {
        LOG_ERROR("Buffer too small for packet. %d < %d.", len,
                header.length);
        return -1;
    }
    ret = socket_read(&header, sizeof(header));
    if(ret < 0)
        return ret;
    if(ret < sizeof(header))
        return -1;      /* Shouldn't happen */
    ret = socket_read(buf, le_to_h_u16(header.length));
    if(ret < 0)
        return ret;
    if(ret < header.length)
        return -1;      /* Shouldn't happen */
    LOG_DEBUG_IO("Got CMSIS-DAP packet. Len %d", header.length);
    return ret;
}

// Read any incoming commands, execute them, and send responses.
// Return zero on success or nothing to do, and <0 on error.
int process_dap_requests(void)
{
    int ret_cmd;

    // Receive from socket, process all commands or until an error occurs.
    while(true) {
        int ret = recv_dap_request(request, sizeof(request));
        if(ret <= 0)
            return ret;

        // DAP_ProcessCommand returns:
        //   number of bytes in response (lower 16 bits)
        //   number of bytes in request (upper 16 bits)
        ret = DAP_ProcessCommand(request, response);
        int request_len = (ret>>16) & 0xFFFF;
        int response_len = ret & 0xFFFF;
        LOG_DEBUG_IO("Processed command. Request len: %d, response len: %d.",
                request_len, response_len);

        ret = send_dap_response(response, response_len);
        if(ret < 0)
            return ret;
    }
}

// ----------------------------------------------------------------------------

int cmsis_dap_tcp_init(int port_number)
{
    int ret = start_server(port_number);
    if(ret < 0) {
        LOG_ERROR("Failed starting server on port %d.", port_number);
    }
    return ret;
}

int cmsis_dap_tcp_process(void)
{
  handle_server();
  handle_client();

  if(client_sockfd != 0)
      return process_dap_requests();
  else
      return 0;
}
