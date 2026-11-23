/* SPDX-License-Identifier: Zlib
 * Raw deflate (RFC 1951) decoder for the jackpotlte loader, after Mark
 * Adler's puff.c (zlib contrib/puff, zlib license). The output buffer is its
 * own history window, so no other buffer is needed: the loader decodes a
 * slot straight into its scratch RAM. No allocation, no recursion; every
 * read and write is bounds-checked.
 */
#include <stdint.h>
#include "inflate.h"

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

struct st {
	uint8_t *out;
	uint64_t outlen, outcnt;
	const uint8_t *in;
	uint64_t inlen, incnt;
	uint32_t bitbuf, bitcnt;
	int err;
};

struct huff {
	uint16_t *count;
	uint16_t *symbol;
};

static uint32_t bits(struct st *s, int need)
{
	uint32_t val = s->bitbuf;

	while (s->bitcnt < (uint32_t)need) {
		if (s->incnt == s->inlen) {
			s->err = JT_INFLATE_EINPUT;
			return 0;
		}
		val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = (uint32_t)(val >> need);
	s->bitcnt -= need;
	return val & ((1u << need) - 1);
}

static int stored(struct st *s)
{
	uint32_t len;

	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->incnt + 4 > s->inlen)
		return JT_INFLATE_EINPUT;
	len = s->in[s->incnt] | ((uint32_t)s->in[s->incnt + 1] << 8);
	if (s->in[s->incnt + 2] != (~len & 0xff) ||
	    s->in[s->incnt + 3] != ((~len >> 8) & 0xff))
		return JT_INFLATE_EDATA;
	s->incnt += 4;
	if (s->incnt + len > s->inlen)
		return JT_INFLATE_EINPUT;
	if (s->outcnt + len > s->outlen)
		return JT_INFLATE_EOUTPUT;
	while (len--)
		s->out[s->outcnt++] = s->in[s->incnt++];
	return 0;
}

static int decode(struct st *s, const struct huff *h)
{
	int len, code = 0, first = 0, count, index = 0;

	for (len = 1; len <= MAXBITS; len++) {
		code |= (int)bits(s, 1);
		if (s->err)
			return -1;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -10;
}

static int construct(struct huff *h, const uint16_t *length, int n)
{
	int symbol, len, left;
	uint16_t offs[MAXBITS + 1];

	for (len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (symbol = 0; symbol < n; symbol++)
		h->count[length[symbol]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0)
			h->symbol[offs[length[symbol]]++] = symbol;
	return left;
}

static const uint16_t lbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint16_t lext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577 };
static const uint16_t dext[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(struct st *s, const struct huff *lencode, const struct huff *distcode)
{
	int symbol;
	uint32_t len, dist;

	do {
		symbol = decode(s, lencode);
		if (s->err)
			return s->err;
		if (symbol < 0)
			return symbol;
		if (symbol < 256) {
			if (s->outcnt == s->outlen)
				return JT_INFLATE_EOUTPUT;
			s->out[s->outcnt++] = (uint8_t)symbol;
		} else if (symbol > 256) {
			symbol -= 257;
			if (symbol >= 29)
				return -10;
			len = lbase[symbol] + bits(s, lext[symbol]);
			symbol = decode(s, distcode);
			if (s->err)
				return s->err;
			if (symbol < 0 || symbol >= 30)
				return -10;
			dist = dbase[symbol] + bits(s, dext[symbol]);
			if (s->err)
				return s->err;
			if (dist > s->outcnt)
				return -11;
			if (s->outcnt + len > s->outlen)
				return JT_INFLATE_EOUTPUT;
			while (len--) {
				s->out[s->outcnt] = s->out[s->outcnt - dist];
				s->outcnt++;
			}
		}
	} while (symbol != 256);
	return 0;
}

static uint16_t lencnt[MAXBITS + 1], lensym[FIXLCODES];
static uint16_t distcnt[MAXBITS + 1], distsym[MAXDCODES];
static uint16_t lengths[MAXCODES];

static int fixed(struct st *s)
{
	struct huff lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
	int symbol;

	for (symbol = 0; symbol < 144; symbol++)
		lengths[symbol] = 8;
	for (; symbol < 256; symbol++)
		lengths[symbol] = 9;
	for (; symbol < 280; symbol++)
		lengths[symbol] = 7;
	for (; symbol < FIXLCODES; symbol++)
		lengths[symbol] = 8;
	construct(&lencode, lengths, FIXLCODES);
	for (symbol = 0; symbol < MAXDCODES; symbol++)
		lengths[symbol] = 5;
	construct(&distcode, lengths, MAXDCODES);
	return codes(s, &lencode, &distcode);
}

static int dynamic(struct st *s)
{
	int nlen, ndist, ncode, index, err;
	struct huff lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
	static const uint8_t order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

	nlen = (int)bits(s, 5) + 257;
	ndist = (int)bits(s, 5) + 1;
	ncode = (int)bits(s, 4) + 4;
	if (s->err)
		return s->err;
	if (nlen > MAXLCODES || ndist > MAXDCODES)
		return -3;
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = (uint16_t)bits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (s->err)
		return s->err;
	err = construct(&lencode, lengths, 19);
	if (err != 0)
		return -4;
	index = 0;
	while (index < nlen + ndist) {
		int symbol, len;

		symbol = decode(s, &lencode);
		if (s->err)
			return s->err;
		if (symbol < 0)
			return symbol;
		if (symbol < 16) {
			lengths[index++] = (uint16_t)symbol;
		} else {
			len = 0;
			if (symbol == 16) {
				if (index == 0)
					return -5;
				len = lengths[index - 1];
				symbol = 3 + (int)bits(s, 2);
			} else if (symbol == 17) {
				symbol = 3 + (int)bits(s, 3);
			} else {
				symbol = 11 + (int)bits(s, 7);
			}
			if (s->err)
				return s->err;
			if (index + symbol > nlen + ndist)
				return -6;
			while (symbol--)
				lengths[index++] = (uint16_t)len;
		}
	}
	if (lengths[256] == 0)
		return -9;
	err = construct(&lencode, lengths, nlen);
	if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
		return -7;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
		return -8;
	return codes(s, &lencode, &distcode);
}

int jt_inflate(uint8_t *dst, uint64_t dstlen, uint64_t *outlen,
    const uint8_t *src, uint64_t srclen)
{
	struct st s;
	int last, type, err;

	s.out = dst;
	s.outlen = dstlen;
	s.outcnt = 0;
	s.in = src;
	s.inlen = srclen;
	s.incnt = 0;
	s.bitbuf = 0;
	s.bitcnt = 0;
	s.err = 0;
	do {
		last = (int)bits(&s, 1);
		type = (int)bits(&s, 2);
		if (s.err) {
			err = s.err;
			break;
		}
		err = type == 0 ? stored(&s) :
		    type == 1 ? fixed(&s) :
		    type == 2 ? dynamic(&s) : -1;
		if (err != 0)
			break;
	} while (!last);
	*outlen = s.outcnt;
	return err;
}
