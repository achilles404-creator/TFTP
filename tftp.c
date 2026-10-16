#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "tftp.h"

packetbuffer_t *append_to_packet(packetbuffer_t *packet, const void *data,
                                 int length, int *offset)
{
    memcpy(packet + *offset, data, length);
    *offset += length;
    return packet;
}

void packet_free(packetbuffer_t *packet)
{
    free(packet);
}

void packet_extract_opcode(const char *buffer, packet_t *packet)
{
    unsigned short op;
    memcpy(&op, buffer, sizeof(op));
    packet->opcode = ntohs(op);
}

static int get_string(const char *buffer, int length, int *pos,
                      char *dest, int dest_size)
{
    int start = *pos;
    int n;

    while (*pos < length && buffer[*pos] != '\0')
        (*pos)++;

    if (*pos >= length)
        return -1;

    n = *pos - start;
    if (n >= dest_size)
        return -1;

    memcpy(dest, buffer + start, n);
    dest[n] = '\0';
    (*pos)++;
    return 0;
}


int packet_parse_rq(const char *buffer, int length, packet_t *packet)
{
    int pos = 2;

    if (!buffer || !packet || length < 6 || length > PACKETSIZE)
        return -1;

    if (get_string(buffer, length, &pos, packet->filename,
                   sizeof(packet->filename)) < 0)
        return -1;

    if (packet->filename[0] == '\0')
        return -1;

    if (get_string(buffer, length, &pos, packet->mode,
                   sizeof(packet->mode)) < 0)
        return -1;

    if (packet->mode[0] == '\0')
        return -1;

    // Reject extra bytes after the mode
    if (pos != length)
        return -1;

    return 0;
}

int packet_parse_data(const char *buffer, int length, packet_t *packet)
{
    unsigned short block;
    int data_len;

    if (!buffer || !packet || length < 4 || length > PACKETSIZE)
        return -1;

    data_len = length - 4;

    memcpy(&block, buffer + 2, sizeof(block));
    packet->blocknum = ntohs(block);

    if (data_len > 0)
        memcpy(packet->data, buffer + 4, data_len);

    packet->data_length = data_len;

    return 0;
}

int packet_parse_ack(const char *buffer, int length, packet_t *packet)
{
    unsigned short block;

    if (!buffer || !packet || length != 4)
        return -1;

    memcpy(&block, buffer + 2, sizeof(block));
    packet->blocknum = ntohs(block);

    return 0;
}

int packet_parse_error(const char *buffer, int length, packet_t *packet)
{
    unsigned short code;
    const char *message_end;
    int message_len;

    if (!buffer || !packet || length < 5 || length > PACKETSIZE)
        return -1;

    // The error message must end with a null byte
    if (buffer[length - 1] != '\0')
        return -1;

    message_end = memchr(buffer + 4, '\0', length - 4);

    // Reject missing terminators and extra bytes after the message
    if (message_end == NULL || message_end != buffer + length - 1)
        return -1;

    message_len = (int)(message_end - (buffer + 4));

    if (message_len >= (int)sizeof(packet->estring))
        return -1;

    memcpy(&code, buffer + 2, sizeof(code));
    packet->ecode = ntohs(code);

    memcpy(packet->estring, buffer + 4, message_len);
    packet->estring[message_len] = '\0';
    packet->estring_length = message_len;

    return 0;
}

int packet_parse(const char *buffer, int length, packet_t *packet)
{
    if (!buffer || !packet || length < 2 || length > PACKETSIZE)
        return -1;

    memset(packet, 0, sizeof(*packet));
    packet_extract_opcode(buffer, packet);

    switch (packet->opcode)
    {
        case OPCODE_RRQ:
        case OPCODE_WRQ:
            return packet_parse_rq(buffer, length, packet);

        case OPCODE_DATA:
            return packet_parse_data(buffer, length, packet);

        case OPCODE_ACK:
            return packet_parse_ack(buffer, length, packet);

        case OPCODE_ERROR:
            return packet_parse_error(buffer, length, packet);

        default:
            return -1;
    }
}

static int form_request(char *buffer, int opcode, const char *filename,
                        const char *mode)
{
    unsigned short op = htons((unsigned short)opcode);
    int pos = 0;
    int file_len;
    int mode_len;

    if (!filename || !mode)
        return -1;

    file_len = (int)strlen(filename);
    mode_len = (int)strlen(mode);

    if (file_len + mode_len + 4 >= PACKETSIZE)
        return -1;

    memcpy(buffer + pos, &op, 2);
    pos += 2;
    memcpy(buffer + pos, filename, file_len + 1);
    pos += file_len + 1;
    memcpy(buffer + pos, mode, mode_len + 1);
    pos += mode_len + 1;

    return pos;
}

int packet_form_rrq(char *buffer, const char *filename, const char *mode)
{
    return form_request(buffer, OPCODE_RRQ, filename, mode);
}

int packet_form_wrq(char *buffer, const char *filename, const char *mode)
{
    return form_request(buffer, OPCODE_WRQ, filename, mode);
}

int packet_form_data(char *buffer, bnum_t block, const char *data, int length)
{
    unsigned short op = htons(OPCODE_DATA);
    unsigned short bn = htons(block);

    if (length < 0 || length > BLIMIT)
        return -1;

    memcpy(buffer, &op, 2);
    memcpy(buffer + 2, &bn, 2);
    if (length > 0)
        memcpy(buffer + 4, data, length);

    return 4 + length;
}

int packet_form_ack(char *buffer, bnum_t block)
{
    unsigned short op = htons(OPCODE_ACK);
    unsigned short bn = htons(block);

    memcpy(buffer, &op, 2);
    memcpy(buffer + 2, &bn, 2);
    return 4;
}

int packet_form_error(char *buffer, ecode_t code, const char *message)
{
    unsigned short op = htons(OPCODE_ERROR);
    unsigned short ec = htons(code);
    int len = message ? (int)strlen(message) : 0;

    if (len + 5 > PACKETSIZE)
        return -1;

    memcpy(buffer, &op, 2);
    memcpy(buffer + 2, &ec, 2);
    if (message)
        memcpy(buffer + 4, message, len);
    buffer[4 + len] = '\0';

    return 5 + len;
}

int tftp_send_packet(int sockfd, const char *buffer, int length,
                     const struct sockaddr_in *peer)
{
    int ret = (int)sendto(sockfd, buffer, length, 0,
                          (const struct sockaddr *)peer, sizeof(*peer));
    return ret == length ? 0 : -1;
}

int tftp_receive_packet(int sockfd, char *buffer, int size,
                        struct sockaddr_in *peer, int timeout_sec)
{
    fd_set readfds;
    struct timeval tv;
    socklen_t len = sizeof(*peer);
    int ret;

    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (ret == 0)
        return -2;
    if (ret < 0)
        return -1;

    ret = (int)recvfrom(sockfd, buffer, size, 0,
                        (struct sockaddr *)peer, &len);
    if (ret < 0)
        return -1;

    return ret;
}

int tftp_send_error(int sockfd, const struct sockaddr_in *peer,
                    ecode_t code, const char *message)
{
    char buffer[PACKETSIZE];
    int len = packet_form_error(buffer, code, message);
    if (len < 0)
        return -1;
    return tftp_send_packet(sockfd, buffer, len, peer);
}

const char *tftp_error_string(ecode_t code)
{
    switch (code)
    {
        case ECODE_0: return "Not defined";
        case ECODE_1: return "File not found";
        case ECODE_2: return "Access violation";
        case ECODE_3: return "Disk full or allocation exceeded";
        case ECODE_4: return "Illegal TFTP operation";
        case ECODE_5: return "Unknown transfer ID";
        case ECODE_6: return "File already exists";
        case ECODE_7: return "No such user";
        default: return "Unknown error";
    }
}

static int receive_expected(int sockfd, char *buffer, int size,
                            struct sockaddr_in *peer, int expected_opcode)
{
    int ret = tftp_receive_packet(sockfd, buffer, size, peer, TIMEOUT);
    if (ret < 0)
        return ret;

    {
        packet_t packet;
        if (packet_parse(buffer, ret, &packet) < 0)
            return -3;
        if (packet.opcode != expected_opcode)
            return -3;
    }

    return ret;
}

int packet_receive_rrq(int sockfd, char *buffer, int size, struct sockaddr_in *peer)
{
    return receive_expected(sockfd, buffer, size, peer, OPCODE_RRQ);
}

int packet_receive_wrq(int sockfd, char *buffer, int size, struct sockaddr_in *peer)
{
    return receive_expected(sockfd, buffer, size, peer, OPCODE_WRQ);
}

int packet_receive_data(int sockfd, char *buffer, int size, struct sockaddr_in *peer)
{
    return receive_expected(sockfd, buffer, size, peer, OPCODE_DATA);
}

int packet_receive_ack(int sockfd, char *buffer, int size, struct sockaddr_in *peer)
{
    return receive_expected(sockfd, buffer, size, peer, OPCODE_ACK);
}

int packet_receive_error(int sockfd, char *buffer, int size, struct sockaddr_in *peer)
{
    return receive_expected(sockfd, buffer, size, peer, OPCODE_ERROR);
}

int packet_receive_invalid(int sockfd, char *buffer, int size, struct sockaddr_in *peer)
{
    return tftp_receive_packet(sockfd, buffer, size, peer, TIMEOUT);
}
