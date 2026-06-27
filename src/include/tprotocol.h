#ifndef TPROTOCOL_H
#define TPROTOCOL_H

#include "debug.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTOCOL_HEADER 0xABCD
#define PROTOCOL_READ_RESTART_MICROSECONDS 10000
#define MAX_PROTOCOL_PAYLOAD_SIZE 2048 + 64

#define SHOW_COMMANDS 0

typedef enum
{
    HEADER_DETECTION,
    COMMAND_READ,
    PAYLOAD_SIZE_READ,
    PAYLOAD_READ_START,
    PAYLOAD_READ_INPROGRESS,
    PAYLOAD_READ_END
} TPParseStep;

typedef struct
{
    uint16_t command_id;
    uint16_t payload_size;
    unsigned char *payload;
    uint16_t bytes_read;
} TransmissionProtocol;

typedef void (*ProtocolCallback)(const TransmissionProtocol *);

void parse_protocol(uint16_t data, ProtocolCallback callback);
void init_protocol_parser();
void terminate_protocol_parser();

#endif // TPROTOCOL_H
