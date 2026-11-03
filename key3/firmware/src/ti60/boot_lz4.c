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
