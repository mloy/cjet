/*
 *The MIT License (MIT)
 *
 * Copyright (c) <2016> <Stephan Gatzka>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef CJET_SOCKET_H
#define CJET_SOCKET_H

#include <stddef.h>

#include "compiler.h"
#include "error_codes.h"
#include "generated/os_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct socket_io_vector {
	const void *iov_base;
	size_t iov_len;
};

/*
 * Platform independent prototypes for socket operations.
 * This functions must be implemented in an OS specific way.
 */
cjet_ssize_t socket_read(socket_type sock, void *buf, size_t count);
cjet_ssize_t socket_writev_with_prefix(socket_type sock, void *buf, size_t len, struct socket_io_vector *io_vec, unsigned int count, int more);
int socket_close(socket_type sock);

enum cjet_system_error get_socket_error(void);
const char *get_socket_error_msg(enum cjet_system_error err);

#ifdef __cplusplus
}
#endif

#endif
