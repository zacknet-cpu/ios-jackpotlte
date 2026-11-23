/* SPDX-License-Identifier: Zlib */
#ifndef JT_INFLATE_H
#define JT_INFLATE_H
#include <stdint.h>

#define JT_INFLATE_EINPUT  2	/* ran out of input */
#define JT_INFLATE_EOUTPUT 1	/* output buffer too small */
#define JT_INFLATE_EDATA   -2	/* stored block length check failed */

/* Raw deflate (no zlib/gzip header). Returns 0 on success, else an error;
 * *outlen is the number of bytes written either way. */
int jt_inflate(uint8_t *dst, uint64_t dstlen, uint64_t *outlen,
    const uint8_t *src, uint64_t srclen);
#endif
