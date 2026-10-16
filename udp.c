#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "udp.h"

static int bind_port(int port)
{
    int sockfd;
    int reuse = 1;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        return -1;

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int udp_bind_server(int port)
{
    return bind_port(port);
}

int udp_rebind_server(int port)
{
    return bind_port(port);
}

int udp_bind_client(void)
{
    return bind_port(0);
}

int udp_send_packet(int sockfd, const char *buffer, int length,
                    const struct sockaddr_in *peer)
{
    return (int)sendto(sockfd, buffer, length, 0,
                       (const struct sockaddr *)peer, sizeof(*peer));
}
