#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "udp.h"
#include "tftp.h"
#include "file.h"

static int peer_is_same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

static int send_request(int sockfd, struct sockaddr_in *server,
                        const char *filename, const char *mode, int is_put)
{
    char buffer[PACKETSIZE];
    int len;

    if (is_put)
        len = packet_form_wrq(buffer, filename, mode);
    else
        len = packet_form_rrq(buffer, filename, mode);

    if (len < 0)
        return -1;

    return tftp_send_packet(sockfd, buffer, len, server);
}

static int receive_data(int sockfd, struct sockaddr_in *server, int fd, const char *mode, netascii_state_t *state)
{
    char buffer[PACKETSIZE];
    char ack[PACKETSIZE];
    bnum_t expected = 2;
    int tries = 0;
    while (tries < TIMEOUT_LIMIT)
    {
        struct sockaddr_in from;
        int len = tftp_receive_packet(sockfd, buffer, sizeof(buffer), &from, TIMEOUT);
        packet_t packet;

        if (len == -2)
        {
            tries++;
            continue;
        }
        if (len < 0)
            return -1;

        if (!peer_is_same(&from, server))
        {
            tftp_send_error(sockfd, &from, ECODE_5, "Unknown transfer ID");
            continue;
        }

        if (packet_parse(buffer, len, &packet) < 0)
            continue;

        if (packet.opcode == OPCODE_ERROR)
        {
            fprintf(stderr, "TFTP error %u: %s\n", packet.ecode, packet.estring);
            return -1;
        }

        if (packet.opcode != OPCODE_DATA)
            continue;

        if (packet.blocknum == expected)
        {
            if (file_append_from_buffer_mode(fd, packet.data, packet.data_length, mode, state) < 0)
                return -1;

            len = packet_form_ack(ack, packet.blocknum);
            if (tftp_send_packet(sockfd, ack, len, &from) < 0)
                return -1;

            if (packet.data_length < BLIMIT)
                return 0;

            expected++;
            if (expected == 0)
                expected = 1;
            tries = 0;
        }
        else if (packet.blocknum < expected)
        {
            len = packet_form_ack(ack, packet.blocknum);
            tftp_send_packet(sockfd, ack, len, &from);
        }
    }

    return -1;
}

static int receive_ack(int sockfd, struct sockaddr_in *server, bnum_t block)
{
    char buffer[PACKETSIZE];
    int tries;

    for (tries = 0; tries < TIMEOUT_LIMIT; tries++)
    {
        struct sockaddr_in from;
        int len = tftp_receive_packet(sockfd, buffer, sizeof(buffer), &from, TIMEOUT);
        packet_t packet;

        if (len == -2)
            continue;
        if (len < 0)
            return -1;

        if (!peer_is_same(&from, server))
        {
            tftp_send_error(sockfd, &from, ECODE_5, "Unknown transfer ID");
            continue;
        }

        if (packet_parse(buffer, len, &packet) < 0)
            continue;

        if (packet.opcode == OPCODE_ACK && packet.blocknum == block)
            return 0;

        if (packet.opcode == OPCODE_ERROR)
        {
            fprintf(stderr, "TFTP error %u: %s\n", packet.ecode, packet.estring);
            return -1;
        }
    }

    return -1;
}

static int send_file(int sockfd, struct sockaddr_in *server, int fd, const char *mode)
{
    char data[BLIMIT];
    char packet[PACKETSIZE];
    bnum_t block = 1;
    int data_len;
    netascii_state_t state = {0};

    while (1)
    {
        if (file_buffer_from_pos_mode(fd, data, BLIMIT, &data_len, mode, &state) < 0)
            return -1;

        {
            int len = packet_form_data(packet, block, data, data_len);
            int tries;
            int ok = 0;

            for (tries = 0; tries < TIMEOUT_LIMIT; tries++)
            {
                if (tftp_send_packet(sockfd, packet, len, server) < 0)
                    return -1;

                if (receive_ack(sockfd, server, block) == 0)
                {
                    ok = 1;
                    break;
                }
            }

            if (!ok)
                return -1;
        }

        if (data_len < BLIMIT)
            break;

        block++;
        if (block == 0)
            block = 1;
    }

    return 0;
}


static int do_get(const char *ip, int port, const char *remote,
                  const char *local, const char *mode)
{
    int sockfd;
    int fd;
    int len;
    int tries;
    struct sockaddr_in server;
    struct sockaddr_in from;
    char buffer[PACKETSIZE];
    packet_t packet;
    netascii_state_t state = {0};

    // Create the local file
    fd = file_open_write(local);
    if (fd < 0)
    {
        perror(local);
        return -1;
    }

    // Create client socket
    sockfd = udp_bind_client();
    if (sockfd < 0)
    {
        perror("socket");
        file_close(fd);
        unlink(local);
        return -1;
    }

    // Set server address
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server.sin_addr) != 1)
    {
        fprintf(stderr, "Invalid server IP\n");
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    // Send RRQ and retry if the server does not respond
    for (tries = 0; tries < TIMEOUT_LIMIT; tries++)
    {
        if (send_request(sockfd, &server, remote, mode, 0) < 0)
        {
            perror("send");
            close(sockfd);
            file_close(fd);
            unlink(local);
            return -1;
        }

        len = tftp_receive_packet(sockfd, buffer, sizeof(buffer),
                                  &from, TIMEOUT);

        if (len == -2)
        {
            printf("Timeout. Retrying RRQ...\n");
            continue;
        }

        if (len < 0)
        {
            fprintf(stderr, "Receive failed\n");
            close(sockfd);
            file_close(fd);
            unlink(local);
            return -1;
        }

        break;
    }

    if (tries == TIMEOUT_LIMIT)
    {
        fprintf(stderr, "Server timeout after retries\n");
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    // Parse the first response
    if (packet_parse(buffer, len, &packet) < 0)
    {
        fprintf(stderr, "Invalid packet from server\n");
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    // Check for server error
    if (packet.opcode == OPCODE_ERROR)
    {
        fprintf(stderr, "TFTP error %u: %s\n",
                packet.ecode, packet.estring);
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    // The first DATA packet gives the transfer port
    server.sin_port = from.sin_port;

    // First packet must be DATA block 1
    if (packet.opcode != OPCODE_DATA || packet.blocknum != 1)
    {
        fprintf(stderr, "Invalid first DATA packet\n");
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    // Write the first data block
    if (file_append_from_buffer_mode(fd, packet.data,
                                     packet.data_length,
                                     mode, &state) < 0)
    {
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    // Acknowledge the first data block
    {
        char ack[PACKETSIZE];

        len = packet_form_ack(ack, 1);
        if (tftp_send_packet(sockfd, ack, len, &server) < 0)
        {
            perror("send ACK");
            close(sockfd);
            file_close(fd);
            unlink(local);
            return -1;
        }
    }

    // Receive the remaining data blocks
    if (packet.data_length == BLIMIT)
    {
        if (receive_data(sockfd, &server, fd, mode, &state) < 0)
        {
            fprintf(stderr, "File transfer failed\n");
            close(sockfd);
            file_close(fd);
            unlink(local);
            return -1;
        }
    }

    // Finish netascii conversion
    if (strcmp(mode, MODE_NETASCII) == 0 &&
        file_netascii_write_finish(fd, &state) < 0)
    {
        close(sockfd);
        file_close(fd);
        unlink(local);
        return -1;
    }

    close(sockfd);
    file_close(fd);

    printf("File received successfully\n");
    return 0;
}

static int do_put(const char *ip, int port, const char *local,
                  const char *remote, const char *mode)
{
    int sockfd;
    int fd;
    int len;
    int tries;
    struct sockaddr_in server;
    struct sockaddr_in from;
    char buffer[PACKETSIZE];
    packet_t packet;

    // Open the local file
    fd = file_open_read(local);
    if (fd < 0)
    {
        perror(local);
        return -1;
    }

    // Create client socket
    sockfd = udp_bind_client();
    if (sockfd < 0)
    {
        perror("socket");
        file_close(fd);
        return -1;
    }

    // Set server address
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server.sin_addr) != 1)
    {
        fprintf(stderr, "Invalid server IP\n");
        close(sockfd);
        file_close(fd);
        return -1;
    }

    // Send WRQ and retry if the server does not respond
    for (tries = 0; tries < TIMEOUT_LIMIT; tries++)
    {
        if (send_request(sockfd, &server, remote, mode, 1) < 0)
        {
            perror("send");
            close(sockfd);
            file_close(fd);
            return -1;
        }

        len = tftp_receive_packet(sockfd, buffer, sizeof(buffer),
                                  &from, TIMEOUT);

        if (len == -2)
        {
            printf("Timeout. Retrying WRQ...\n");
            continue;
        }

        if (len < 0)
        {
            fprintf(stderr, "Receive failed\n");
            close(sockfd);
            file_close(fd);
            return -1;
        }

        break;
    }

    if (tries == TIMEOUT_LIMIT)
    {
        fprintf(stderr, "Server timeout after retries\n");
        close(sockfd);
        file_close(fd);
        return -1;
    }

    // Parse the first response
    if (packet_parse(buffer, len, &packet) < 0)
    {
        fprintf(stderr, "Invalid packet from server\n");
        close(sockfd);
        file_close(fd);
        return -1;
    }

    // Check for server error
    if (packet.opcode == OPCODE_ERROR)
    {
        fprintf(stderr, "TFTP error %u: %s\n",
                packet.ecode, packet.estring);
        close(sockfd);
        file_close(fd);
        return -1;
    }

    // The response gives the transfer port
    server.sin_port = from.sin_port;

    // WRQ must receive ACK block 0
    if (packet.opcode != OPCODE_ACK || packet.blocknum != 0)
    {
        fprintf(stderr, "Invalid ACK from server\n");
        close(sockfd);
        file_close(fd);
        return -1;
    }

    // Send the file data
    if (send_file(sockfd, &server, fd, mode) < 0)
    {
        fprintf(stderr, "File transfer failed\n");
        close(sockfd);
        file_close(fd);
        return -1;
    }

    close(sockfd);
    file_close(fd);

    printf("File sent successfully\n");
    return 0;
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  connect <ip> [port]      Connect to server\n");
    printf("  get <file> [local]      Receive file from server\n");
    printf("  put <file> [remote]     Send file to server\n");
    printf("  mode <octet|netascii>   Set transfer mode\n");
    printf("  bye / quit              Exit client\n");
    printf("  help                    Show this help\n");
}

int main(int argc, char *argv[])
{
    char ip[64] = "";
    int port = 0;
    char mode[32] = MODE_OCTET;

    if (argc == 6)
    {
        const char *remote = argv[3];
        const char *local = argv[4];
        if (strcmp(argv[5], "get") == 0)
            return do_get(argv[1], atoi(argv[2]), remote, local, mode) == 0 ? 0 : 1;
        if (strcmp(argv[5], "put") == 0)
            return do_put(argv[1], atoi(argv[2]), remote, local, mode) == 0 ? 0 : 1;
    }

    if (argc != 1)
    {
        fprintf(stderr, "Usage: %s <server ip> <port> <filename> <output filename> [get|put]\n", argv[0]);
        fprintf(stderr, "   or: %s\n", argv[0]);
        return 1;
    }

    print_help();

    while (1)
    {
        char line[256];
        char command[32];
        char arg1[128];
        char arg2[128];
        int count;

        printf("tftp> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        count = sscanf(line, "%31s %127s %127s", command, arg1, arg2);
        if (count <= 0)
            continue;

        if (strcmp(command, "help") == 0)
        {
            print_help();
        }
        else if (strcmp(command, "connect") == 0 && count >= 2)
        {
            strncpy(ip, arg1, sizeof(ip) - 1);
            ip[sizeof(ip) - 1] = '\0';
            if (count >= 3)
                port = atoi(arg2);
            else if (port == 0)
                port = 69;
            printf("Connected to %s:%d\n", ip, port);
        }
        else if (strcmp(command, "mode") == 0 && count >= 2)
        {
            if (strcmp(arg1, MODE_OCTET) == 0 || strcmp(arg1, MODE_NETASCII) == 0)
            {
                strncpy(mode, arg1, sizeof(mode) - 1);
                mode[sizeof(mode) - 1] = '\0';
                printf("Mode set to %s\n", mode);
            }
            else
            {
                printf("Unsupported mode. Use octet or netascii.\n");
            }
        }
        else if (strcmp(command, "get") == 0 && count >= 2)
        {
            if (ip[0] == '\0' || port == 0)
                printf("Connect to a server first\n");
            else
            {
                const char *local = count >= 3 ? arg2 : arg1;
                do_get(ip, port, arg1, local, mode);
            }
        }
        else if (strcmp(command, "put") == 0 && count >= 2)
        {
            if (ip[0] == '\0' || port == 0)
                printf("Connect to a server first\n");
            else
            {
                const char *remote = count >= 3 ? arg2 : arg1;
                do_put(ip, port, arg1, remote, mode);
            }
        }
        else if (strcmp(command, "bye") == 0 || strcmp(command, "quit") == 0)
        {
            break;
        }
        else
        {
            printf("Invalid command. Type help.\n");
        }
    }

    return 0;
}
