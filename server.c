#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "file.h"
#include "udp.h"
#include "tftp.h"

static volatile sig_atomic_t next_port = SERVER_PORT_START;

static void reap_children(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static int peer_is_same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

static int send_data_and_wait_ack(int sockfd, struct sockaddr_in *peer,
                                  bnum_t block, const char *data, int data_len)
{
    char sendbuf[PACKETSIZE];
    char recvbuf[PACKETSIZE];
    int sendlen = packet_form_data(sendbuf, block, data, data_len);
    int tries;

    for (tries = 0; tries < TIMEOUT_LIMIT; tries++)
    {
        struct sockaddr_in from;
        int len;
        packet_t packet;

        if (tftp_send_packet(sockfd, sendbuf, sendlen, peer) < 0)
            return -1;

        len = tftp_receive_packet(sockfd, recvbuf, sizeof(recvbuf), &from, TIMEOUT);
        if (len == -2)
            continue;
        if (len < 0)
            return -1;
        if (!peer_is_same(&from, peer))
        {
            tftp_send_error(sockfd, &from, ECODE_5, "Unknown transfer ID");
            continue;
        }

        if (packet_parse(recvbuf, len, &packet) == 0 &&
            packet.opcode == OPCODE_ACK && packet.blocknum == block)
            return 0;

        if (packet.opcode == OPCODE_ERROR)
            return -1;
    }

    return -1;
}

static int receive_data_and_ack(int sockfd, struct sockaddr_in *peer,
                                bnum_t expected_block, char *data, int *data_len)
{
    char recvbuf[PACKETSIZE];
    char ackbuf[PACKETSIZE];
    int tries;

    for (tries = 0; tries < TIMEOUT_LIMIT; tries++)
    {
        struct sockaddr_in from;
        int len = tftp_receive_packet(sockfd, recvbuf, sizeof(recvbuf), &from, TIMEOUT);
        packet_t packet;

        if (len == -2)
            continue;
        if (len < 0)
            return -1;
        if (!peer_is_same(&from, peer))
        {
            tftp_send_error(sockfd, &from, ECODE_5, "Unknown transfer ID");
            continue;
        }

        if (packet_parse(recvbuf, len, &packet) < 0)
            continue;

        if (packet.opcode == OPCODE_ERROR)
            return -1;

        if (packet.opcode != OPCODE_DATA)
            continue;

        if (packet.blocknum == expected_block)
        {
            memcpy(data, packet.data, packet.data_length);
            *data_len = packet.data_length;
            len = packet_form_ack(ackbuf, expected_block);
            tftp_send_packet(sockfd, ackbuf, len, peer);
            return 0;
        }

        if (packet.blocknum < expected_block)
        {
            len = packet_form_ack(ackbuf, packet.blocknum);
            tftp_send_packet(sockfd, ackbuf, len, peer);
        }
    }

    return -1;
}

static int server_get(int sockfd, struct sockaddr_in *peer,
                      const char *filename, const char *mode)
{
    int fd;
    bnum_t block = 1;
    char data[BLIMIT];
    int data_len;
    netascii_state_t state = {0};

    fd = file_open_read(filename);
    if (fd < 0)
    {
        tftp_send_error(sockfd, peer, ECODE_1, "File not found");
        return -1;
    }

    while (1)
    {
        if (file_buffer_from_pos_mode(fd, data, BLIMIT, &data_len, mode, &state) < 0)
        {
            tftp_send_error(sockfd, peer, ECODE_0, "File read error");
            file_close(fd);
            return -1;
        }

        if (send_data_and_wait_ack(sockfd, peer, block, data, data_len) < 0)
        {
            file_close(fd);
            return -1;
        }

        if (data_len < BLIMIT)
            break;

        block++;
        if (block == 0)
            block = 1;
    }

    (void)mode;
    file_close(fd);
    return 0;
}


static int server_put(int sockfd, struct sockaddr_in *peer,
                      const char *filename, const char *mode)
{
    int fd;
    bnum_t block = 1;
    char data[BLIMIT];
    int data_len;
    netascii_state_t state = {0};
    char ackbuf[PACKETSIZE];
    int acklen;

    // Create the destination file
    fd = file_open_write(filename);
    
    if (fd < 0)
    {
        if (errno == EEXIST)
            tftp_send_error(sockfd, peer, ECODE_6, "File already exists");
        else if (errno == EACCES)
            tftp_send_error(sockfd, peer, ECODE_2, "Access violation");
        else
            tftp_send_error(sockfd, peer, ECODE_0, "Cannot create file");

        return -1;
    }

    // Send ACK block 0
    acklen = packet_form_ack(ackbuf, 0);

    if (tftp_send_packet(sockfd, ackbuf, acklen, peer) < 0)
    {
        file_close(fd);
        unlink(filename);
        return -1;
    }

    // Receive and write file data
    while (1)
    {
        if (receive_data_and_ack(sockfd, peer, block,
                                 data, &data_len) < 0)
        {
            file_close(fd);
            unlink(filename);
            return -1;
        }

        if (file_append_from_buffer_mode(fd, data, data_len,
                                         mode, &state) < 0)
        {
            tftp_send_error(sockfd, peer, ECODE_3,
                            "Disk full or allocation exceeded");

            file_close(fd);
            unlink(filename);
            return -1;
        }

        // A short block marks the end of the transfer
        if (data_len < BLIMIT)
            break;

        block++;
        if (block == 0)
            block = 1;
    }

    // Finish netascii conversion
    if (strcmp(mode, MODE_NETASCII) == 0 &&
        file_netascii_write_finish(fd, &state) < 0)
    {
        file_close(fd);
        unlink(filename);
        return -1;
    }

    file_close(fd);

    return 0;
}

static void handle_request(int sockfd, struct sockaddr_in *peer,
                           const char *buffer, int length)
{
    packet_t packet;

    if (packet_parse(buffer, length, &packet) < 0)
    {
        tftp_send_error(sockfd, peer, ECODE_4, "Illegal TFTP operation");
        return;
    }

    if (strcmp(packet.mode, MODE_MAIL) == 0)
    {
        tftp_send_error(sockfd, peer, ECODE_4, "Mail mode is not supported");
        return;
    }

    if (strcmp(packet.mode, MODE_OCTET) != 0 &&
        strcmp(packet.mode, MODE_NETASCII) != 0)
    {
        tftp_send_error(sockfd, peer, ECODE_4, "Unsupported transfer mode");
        return;
    }

    if (packet.opcode == OPCODE_RRQ)
        server_get(sockfd, peer, packet.filename, packet.mode);
    else if (packet.opcode == OPCODE_WRQ)
        server_put(sockfd, peer, packet.filename, packet.mode);
    else
        tftp_send_error(sockfd, peer, ECODE_4, "Illegal TFTP operation");
}

int main(int argc, char *argv[])
{
    int sockfd;
    int port;
    char buffer[PACKETSIZE];

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        return 1;
    }

    port = atoi(argv[1]);
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }

    sockfd = udp_bind_server(port);
    if (sockfd < 0)
    {
        perror("bind");
        return 1;
    }

    signal(SIGCHLD, reap_children);

    while (1)
    {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int n;
        pid_t pid;
        int child_port = next_port++;

        n = (int)recvfrom(sockfd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&client, &len);
        if (n < 0)
            continue;

        pid = fork();
        if (pid < 0)
            continue;

        if (pid == 0)
        {
            int child_sock;
            close(sockfd);

            child_sock = -1;
            while (child_port <= 65535)
            {
                child_sock = udp_rebind_server(child_port);
                if (child_sock >= 0)
                    break;
                child_port++;
            }

            if (child_sock < 0)
                _exit(1);

            handle_request(child_sock, &client, buffer, n);
            close(child_sock);
            _exit(0);
        }
    }

    return 0;
}
