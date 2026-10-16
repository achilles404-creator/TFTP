#ifndef TFTP_UDP_H
#define TFTP_UDP_H

#include <netinet/in.h>

int udp_bind_server(int port);
int udp_rebind_server(int port);
int udp_bind_client(void);
int udp_send_packet(int sockfd, const char *buffer, int length,
                    const struct sockaddr_in *peer);

#endif
