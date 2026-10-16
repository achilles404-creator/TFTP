#ifndef TFTP_FILE_H
#define TFTP_FILE_H

#include "tftp.h"

int file_open_read(const char *filename);
int file_buffer_from_pos(int fd, char *buffer, int maxlen, int *length);
int file_open_write(const char *filename);
int file_append_from_buffer(int fd, const char *buffer, int length);
int file_close(int fd);

typedef struct
{
    int pending_valid;
    unsigned char pending;
    int pending_cr;
} netascii_state_t;

int file_buffer_from_pos_mode(int fd, char *buffer, int maxlen, int *length,
                              const char *mode, netascii_state_t *state);
int file_append_from_buffer_mode(int fd, const char *buffer, int length,
                                 const char *mode, netascii_state_t *state);
int file_netascii_write_finish(int fd, netascii_state_t *state);

#endif
