#ifndef COMMANDS_H
#define COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

// Set up the serial console and start it running.
void commands_init_serial(void);

// Runs one socket console connection's REPL until the peer disconnects.
// f is the fdopen()ed connection stream; caller owns it.
void process_socket_commands(FILE *f);

#ifdef __cplusplus
}
#endif

#endif // COMMANDS_H
