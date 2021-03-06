/* retry.c -- keep trying write system calls
 *
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: retry.c,v 1.27 2010/06/28 12:05:57 brong Exp $
 */

#include <config.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "retry.h"

/*
 * Keep calling the read() system call with 'fd', 'buf', and 'nbyte'
 * until all the data is read in or an error occurs.
 */
int retry_read(int fd, void *buf, size_t nbyte)
{
    int n;
    int nread = 0;

    if (nbyte == 0) return 0;

    for (;;) {
	n = read(fd, buf, nbyte);
	if (n == 0) {
	    /* end of file */
	    return -1;
	}

	if (n == -1) {
	    if (errno == EINTR || errno == EAGAIN) continue;
	    return -1;
	}

	nread += n;

	if (((size_t) n) >= nbyte) return nread;

	buf += n;
	nbyte -= n;
    }
}

/*
 * Keep calling the write() system call with 'fd', 'buf', and 'nbyte'
 * until all the data is written out or an error occurs.
 */
int retry_write(int fd, const void *buf, size_t nbyte)
{
    int n;
    int written = 0;

    if (nbyte == 0) return 0;

    for (;;) {
	n = write(fd, buf, nbyte);
	if (n == -1) {
	    if (errno == EINTR) continue;
	    return -1;
	}

	written += n;

	if (((size_t) n) >= nbyte) return written;

	buf += n;
	nbyte -= n;
    }
}

	
/*
 * Keep calling the writev() system call with 'fd', 'iov', and 'iovcnt'
 * until all the data is written out or an error occurs.
 *
 * Now no longer destructive of parameters!
 */
int
retry_writev(fd, srciov, iovcnt)
int fd;
struct iovec *srciov;
int iovcnt;
{
    int n;
    int i;
    int written = 0;
    struct iovec *iov, *baseiov;
    static int iov_max =
#ifdef MAXIOV
	MAXIOV
#else
#ifdef IOV_MAX
	IOV_MAX
#else
	8192
#endif
#endif
	;

    baseiov = iov = (struct iovec *)xmalloc(iovcnt * sizeof(struct iovec));
    for (i = 0; i < iovcnt; i++) {
	iov[i].iov_base = srciov[i].iov_base;
	iov[i].iov_len = srciov[i].iov_len;
    }

    for (;;) {
	while (iovcnt && iov[0].iov_len == 0) {
	    iov++;
	    iovcnt--;
	}

	if (!iovcnt) goto done;

	n = writev(fd, iov, iovcnt > iov_max ? iov_max : iovcnt);
	if (n == -1) {
	    if (errno == EINVAL && iov_max > 10) {
		iov_max /= 2;
		continue;
	    }
	    if (errno == EINTR) continue;
	    written = -1;
	    goto done;
	}

	written += n;

	for (i = 0; i < iovcnt; i++) {
	    if (iov[i].iov_len > (size_t) n) {
		iov[i].iov_base = (char *)iov[i].iov_base + n;
		iov[i].iov_len -= n;
		break;
	    }
	    n -= iov[i].iov_len;
	    iov[i].iov_len = 0;
	}

	if (i == iovcnt) goto done;
    }

done:
    free(baseiov);
    return written;
}

	
