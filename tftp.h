#ifndef TFTP_H
#define TFTP_H

#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BLIMIT       512
#define PACKETSIZE   516
#define TIMEOUT      1
#define TIMEOUT_LIMIT 10
#define SERVER_PORT_START 20000

#define OPCODE_RRQ   1
#define IS_RRQ(op)   ((op) == OPCODE_RRQ)
#define OPCODE_WRQ   2
#define IS_WRQ(op)   ((op) == OPCODE_WRQ)
#define OPCODE_DATA  3
#define IS_DATA(op)   ((op) == OPCODE_DATA)
#define OPCODE_ACK   4
#define IS_ACK(op)   ((op) == OPCODE_ACK)
#define OPCODE_ERROR 5
#define IS_ERROR(op) ((op) == OPCODE_ERROR)

#define ECODE_NONE 8
#define ECODE_0 0
#define ESTRING_0 "Not defined, see error message(if any)."
#define ECODE_1 1
#define ESTRING_1 "File not found."
#define ECODE_2 2
#define ESTRING_2 "Access violation."
#define ECODE_3 3
#define ESTRING_3 "Disk full or allocation exceeded."
#define ECODE_4 4
#define ESTRING_4 "Illegal TFTP operation."
#define ECODE_5 5
#define ESTRING_5 "Unknown transfer ID."
#define ECODE_6 6
#define ESTRING_6 "File already exists."
#define ECODE_7 7
#define ESTRING_7 "No such user."

#define MODE_NETASCII "netascii"
#define MODE_OCTET    "octet"
#define MODE_MAIL     "mail"

typedef unsigned short opcode_t;
typedef unsigned short ecode_t;
typedef unsigned short bnum_t;
typedef char packetbuffer_t;

typedef struct tftp_packet
{
    char filename[PACKETSIZE];
    opcode_t opcode;
    char mode[PACKETSIZE];
    char data[BLIMIT];
    int data_length;
    bnum_t blocknum;
    ecode_t ecode;
    char estring[PACKETSIZE];
    int estring_length;
} packet_t;

/* Packet parsing */
int packet_parse(const char *buffer, int length, packet_t *packet);
void packet_extract_opcode(const char *buffer, packet_t *packet);

int packet_parse_rq(const char *buffer, int length, packet_t *packet);
int packet_parse_data(const char *buffer, int length, packet_t *packet);
int packet_parse_ack(const char *buffer, int length, packet_t *packet);
int packet_parse_error(const char *buffer, int length, packet_t *packet);


/* Packet formation */
int packet_form_rrq(char *buffer, const char *filename, const char *mode);
int packet_form_wrq(char *buffer, const char *filename, const char *mode);
int packet_form_data(char *buffer, bnum_t block, const char *data, int length);
int packet_form_ack(char *buffer, bnum_t block);
int packet_form_error(char *buffer, ecode_t code, const char *message);
packetbuffer_t *append_to_packet(packetbuffer_t *packet, const void *data, int length, int *offset);
void packet_free(packetbuffer_t *packet);

/* Packet receive helpers */
int packet_receive_rrq(int sockfd, char *buffer, int size, struct sockaddr_in *peer);
int packet_receive_wrq(int sockfd, char *buffer, int size, struct sockaddr_in *peer);
int packet_receive_data(int sockfd, char *buffer, int size, struct sockaddr_in *peer);
int packet_receive_ack(int sockfd, char *buffer, int size, struct sockaddr_in *peer);
int packet_receive_error(int sockfd, char *buffer, int size, struct sockaddr_in *peer);
int packet_receive_invalid(int sockfd, char *buffer, int size, struct sockaddr_in *peer);

/* Common send/receive */
int tftp_send_packet(int sockfd, const char *buffer, int length,
                     const struct sockaddr_in *peer);
int tftp_receive_packet(int sockfd, char *buffer, int size,
                        struct sockaddr_in *peer, int timeout_sec);
int tftp_send_error(int sockfd, const struct sockaddr_in *peer,
                    ecode_t code, const char *message);
const char *tftp_error_string(ecode_t code);

#endif
