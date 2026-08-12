#ifndef COMMANDS_H
#define COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "tls_transport.h"

// Set up the serial console and start it running.
void commands_init_serial(void);

// Runs one socket console connection's REPL until the peer disconnects.
// f is the connection's output stream (transport_fopen()ed); t is the same
// connection's transport handle, needed for the raw peer-closed check. The
// caller owns both.
void process_socket_commands(FILE *f, transport_handle_t t);

#ifdef __cplusplus
}
#endif

#endif // COMMANDS_H
