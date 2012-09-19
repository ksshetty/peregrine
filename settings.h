#ifndef __SETTINGS_H__
#define __SETTINGS_H__
#include "packetformat.h"

#define RECVER_PORT 2112
#define SENDER_PORT 2113
#define PACKET_SIZE 1470
#define ACK_RETRANSMIT 5
#define ACK_TERMINATION_NUMBER 30

#define TYPE_ACK  0
#define TYPE_NACK 1

#define RECVR_METADATA_WAIT 20   //Milliseconds
#define RECVR_FILE "data"
typedef struct Packet_Metadata_ PacketMetadata;
typedef struct Packet_Data_ PacketData;
typedef struct Packet_CumulativeNack_ PacketCumulativeNack;
typedef struct Packet_Ack_ PacketAck;
#endif
