#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "file.h"

int file_open_read(const char *filename)
{
    return open(filename, O_RDONLY);
}

int file_buffer_from_pos(int fd, char *buffer, int maxlen, int *length)
{
    int ret = (int)read(fd, buffer, maxlen);
    if (ret < 0)
        return -1;

    *length = ret;
    return 0;
}

int file_open_write(const char *filename)
{
    return open(filename, O_WRONLY | O_CREAT | O_EXCL, 0666);
}

int file_append_from_buffer(int fd, const char *buffer, int length)
{
    int total = 0;

    while (total < length)
    {
        int ret = (int)write(fd, buffer + total, length - total);
        if (ret < 0)
            return -1;
        if (ret == 0)
            return -1;
        total += ret;
    }

    return 0;
}

int file_close(int fd)
{
    return close(fd);
}


int file_buffer_from_pos_mode(int fd, char *buffer, int maxlen, int *length,
                              const char *mode, netascii_state_t *state)
{
    int out = 0;

    if (strcmp(mode, MODE_OCTET) == 0)
        return file_buffer_from_pos(fd, buffer, maxlen, length);

    if (strcmp(mode, MODE_NETASCII) != 0)
        return -1;

    while (out < maxlen)
    {
        unsigned char c;

        if (state->pending_valid)
        {
            c = state->pending;
            state->pending_valid = 0;
            buffer[out++] = (char)c;
            continue;
        }

        if (read(fd, &c, 1) != 1)
            break;

        if (c == '\n')
        {
            buffer[out++] = '\r';
            if (out == maxlen)
            {
                state->pending_valid = 1;
                state->pending = '\n';
            }
            else
                buffer[out++] = '\n';
        }
        else if (c == '\r')
        {
            buffer[out++] = '\r';
            if (out == maxlen)
            {
                state->pending_valid = 1;
                state->pending = '\0';
            }
            else
                buffer[out++] = '\0';
        }
        else
        {
            buffer[out++] = (char)c;
        }
    }

    *length = out;
    return 0;
}

int file_append_from_buffer_mode(int fd, const char *buffer, int length,
                                 const char *mode, netascii_state_t *state)
{
    int i = 0;

    if (strcmp(mode, MODE_OCTET) == 0)
        return file_append_from_buffer(fd, buffer, length);

    if (strcmp(mode, MODE_NETASCII) != 0)
        return -1;

    while (i < length)
    {
        unsigned char c = (unsigned char)buffer[i++];

        if (state->pending_cr)
        {
            unsigned char out;
            if (c == '\n')
                out = '\n';
            else if (c == '\0')
                out = '\r';
            else
            {
                out = '\r';
                if (file_append_from_buffer(fd, (char *)&out, 1) < 0)
                    return -1;
                state->pending_cr = 0;
                if (file_append_from_buffer(fd, (char *)&c, 1) < 0)
                    return -1;
                continue;
            }

            if (file_append_from_buffer(fd, (char *)&out, 1) < 0)
                return -1;
            state->pending_cr = 0;
            continue;
        }

        if (c == '\r')
        {
            state->pending_cr = 1;
        }
        else
        {
            if (file_append_from_buffer(fd, (char *)&c, 1) < 0)
                return -1;
        }
    }

    return 0;
}

int file_netascii_write_finish(int fd, netascii_state_t *state)
{
    if (state->pending_cr)
    {
        unsigned char c = '\r';
        if (file_append_from_buffer(fd, (char *)&c, 1) < 0)
            return -1;
        state->pending_cr = 0;
    }
    return 0;
}
