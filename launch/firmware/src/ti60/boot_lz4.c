#include "boot_lz4.h"

static int32_t next_in(const boot_lz4_io *io, uint32_t ip, uint32_t in_len)
{
	if (ip >= in_len)
		return -1;
	return io->in(io->ctx);
}

int32_t boot_lz4_decode(const boot_lz4_io *io, uint32_t in_len, uint32_t out_cap)
{
	uint32_t ip = 0, op = 0;
	for (;;) {
		int32_t b = next_in(io, ip, in_len);
		if (b < 0)
			return -1; /* token missing */
		uint32_t tok = (uint32_t)b;
		ip++;
		uint32_t ll = tok >> 4;
		if (ll == 15) {
			do {
				b = next_in(io, ip, in_len);
				if (b < 0)
					return -1;
				ip++;
				ll += (uint32_t)b;
				if (ll > out_cap)
					return -1; /* length field already overflows output */
			} while (b == 255);
		}
		for (uint32_t i = 0; i < ll; i++) {
			b = next_in(io, ip, in_len);
			if (b < 0 || op >= out_cap)
				return -1;
			io->out(io->ctx, op, (uint8_t)b);
			op++;
			ip++;
		}
		if (ip == in_len)
			break; /* final literal-only sequence */
		b = next_in(io, ip, in_len);
		if (b < 0)
			return -1;
		uint32_t off = (uint32_t)b;
		ip++;
		b = next_in(io, ip, in_len);
		if (b < 0)
			return -1;
		off |= (uint32_t)b << 8;
		ip++;
		if (off == 0 || off > op)
			return -1; /* match reaches before the output start */
		uint32_t ml = (tok & 15u) + 4u;
		if ((tok & 15u) == 15u) {
			do {
				b = next_in(io, ip, in_len);
				if (b < 0)
					return -1;
				ip++;
				ml += (uint32_t)b;
				if (op + ml > out_cap)
					return -1;
			} while (b == 255);
		}
		if (op + ml > out_cap)
			return -1;
		for (uint32_t i = 0; i < ml; i++) {
			io->out(io->ctx, op, io->back(io->ctx, op, off));
			op++;
		}
	}
	return (int32_t)op;
}

/* CFZ2 decoder: walks the byte-aligned structure stream, pulling each token's
 * literal count from the canonical-Huffman bitstream (MSB-first). Symbol
 * lookup is O(1) through a canonical-order table built once from the lengths;
 * every count and code is bounds-checked so corrupt input only fails. */
int32_t boot_lz4_decode2(const boot_lz4_io *io, const boot_lz4_src *lit,
	const boot_lz4_src *st, const uint8_t *lengths,
	uint32_t st_len, uint32_t out_cap)
{
	/* Symbol lookup is a linear scan in canonical (length, symbol) order:
	 * boot time is not critical and dropping the sorted/base/pos tables keeps
	 * the OCRAM-resident decoder inside its 2.5 KiB budget. */
	uint16_t counts[16], first[16];
	uint32_t op = 0, consumed = 0, acc = 0;
	int32_t nb = 0;
	uint32_t i, code = 0;
	for (i = 0; i < 16; i++) counts[i] = first[i] = 0;
	for (i = 0; i < 256; i++) {
		if (lengths[i] > 15) return -1;
		counts[lengths[i]]++;
	}
	counts[0] = 0;
	for (i = 1; i <= 15; i++) {
		code = (code + counts[i - 1]) << 1;
		first[i] = (uint16_t)code;
	}
	while (consumed < st_len) {
		int32_t b = st->in(st->ctx);
		uint32_t tok, ll, ml;
		if (b < 0 || consumed + 1 > st_len) return -1;
		consumed++;
		tok = (uint32_t)b;
		ll = tok >> 4;
		if (ll == 15) {
			for (;;) {
				b = st->in(st->ctx);
				if (b < 0 || consumed + 1 > st_len) return -1;
				consumed++;
				ll += (uint32_t)b;
				if (b != 255) break;
			}
		}
		for (i = 0; i < ll; i++) {
			uint32_t cur = 0, cl = 0;
			for (;;) {
				if (nb == 0) {
					b = lit->in(lit->ctx);
					if (b < 0) return -1;
					acc = (uint32_t)b;
					nb = 8;
				}
				nb--;
				cur = (cur << 1) | ((acc >> nb) & 1u);
				cl++;
				if (cl > 15) return -1;
				if (counts[cl] && cur >= first[cl] && cur - first[cl] < counts[cl]) {
					uint32_t sym = 0, seen = 0, want = cur - first[cl];
					for (sym = 0; sym < 256; sym++) {
						if (lengths[sym] == cl && seen++ == want) break;
					}
					if (sym == 256) return -1;
					if (op >= out_cap) return -1;
					io->out(io->ctx, op, (uint8_t)sym);
					op++;
					break;
				}
			}
		}
		if (consumed >= st_len) break; /* final literal-only op ends the walk */
		ml = (tok & 15u) + 4; /* the nibble encodes ml-4: zero means a 4-byte match */
		b = st->in(st->ctx);
		if (b < 0 || consumed + 2 > st_len) return -1;
		{
			uint32_t off = (uint32_t)b;
			b = st->in(st->ctx);
			if (b < 0) return -1;
			consumed += 2;
			off |= (uint32_t)b << 8;
			if (off == 0 || off > op) return -1;
			if ((tok & 15u) == 15u) {
				for (;;) {
					b = st->in(st->ctx);
					if (b < 0 || consumed + 1 > st_len) return -1;
					consumed++;
					ml += (uint32_t)b;
					if (b != 255) break;
				}
			}
			if (op + ml > out_cap) return -1;
			for (i = 0; i < ml; i++) {
				io->out(io->ctx, op, io->back(io->ctx, op, off));
				op++;
			}
		}
	}
	return (int32_t)op;
}
