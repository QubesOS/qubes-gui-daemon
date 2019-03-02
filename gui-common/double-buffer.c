/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static char *buffer;
static int buffer_size;
static int data_offset;
static int data_count;
#define BUFFER_SIZE_MIN 8192
#define BUFFER_SIZE_MAX 10000000
void double_buffer_init(void)
{
    buffer = malloc(BUFFER_SIZE_MIN);
    if (!buffer) {
        fprintf(stderr, "malloc");
        exit(1);
    }
    buffer_size = BUFFER_SIZE_MIN;
}
// We assume that the only common case when we need to enlarge the buffer
// is when we send a single large blob (after Ctrl-Shift-V).
// Thus, we optimize for double_buffer_substract(), as it can be called
// many times; malloc(newsize) in double_buffer_append() should be rare
// in normal circumstances.

void double_buffer_append(char *buf, int size)
{
    int __attribute__((unused)) ignore;

    if ((unsigned)size > BUFFER_SIZE_MAX) {
        fprintf(stderr, "double_buffer_append: req_size=%d\n", size);
        ignore = system
            ("/usr/bin/xmessage -button OK:2 'Suspiciously large buffer, terminating...'");
        exit(1);
    }
    if (size + data_offset + data_count > buffer_size) {
        int newsize = data_count + size + BUFFER_SIZE_MIN;
        char *newbuf;
        if (newsize > BUFFER_SIZE_MAX) {
            fprintf(stderr,
                "double_buffer_append: offset=%d, data_count=%d, req_size=%d\n",
                data_offset, data_count, size);
            ignore = system
                ("/usr/bin/xmessage -button OK:2 'Out of buffer space (AppVM refuses to read data?), terminating...'");
            exit(1);
        }
        newbuf = malloc(newsize);
        if (!newbuf) {
            fprintf(stderr, "malloc");
            exit(1);
        }
        memcpy(newbuf, buffer + data_offset, data_count);
        free(buffer);
        buffer = newbuf;
        buffer_size = newsize;
        data_offset = 0;
    }
    memcpy(buffer + data_offset + data_count, buf, size);
    data_count += size;
}

int double_buffer_datacount(void)
{
    return data_count;
}

char *double_buffer_data(void)
{
    return buffer + data_offset;
}

void double_buffer_substract(int count)
{
    if (count > data_count) {
        fprintf(stderr,
            "double_buffer_substract, count=%d, data_count=%d\n",
            count, data_count);
        exit(1);
    }
    data_count -= count;
    data_offset += count;
    if (data_count == 0) {
        if (buffer_size > BUFFER_SIZE_MIN) {
            free(buffer);
            buffer = malloc(BUFFER_SIZE_MIN);
            if (!buffer) {
                fprintf(stderr, "malloc");
                exit(1);
            }

        }
        data_offset = 0;
        buffer_size = BUFFER_SIZE_MIN;
    }
}
