/* crc32.h
 */

#ifndef CRC32_H
#define CRC32_H
#include "util.h"
#include <sys/uio.h>
#ifdef HAVE_STDINT_H
# include <stdint.h>
#else
# include <inttypes.h>
#endif

uint32_t crc32_map(const char *base, unsigned len);
uint32_t crc32_buf(struct buf *buf);
uint32_t crc32_cstring(const char *buf);
uint32_t crc32_iovec(struct iovec *iov, int iovcnt);

#endif
