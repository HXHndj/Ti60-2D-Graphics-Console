#ifndef BOOT_LZ4_H
#define BOOT_LZ4_H
#include <stdint.h>
/* LZ4 block decoder for the CFZ1 boot ROM container. Pure logic, no memory
 * of its own: input, output and back-references are bound through callbacks
 * so the same code runs in the OCRAM loader (ROM word reader / DDR byte
 * writer) and in host self-tests (memory-backed bindings). Semantics are the
 * exact inverse of tools/console_firmware.py lz4_compress: byte-wise copies,
 * in_len is byte-granular, the stream must end right after a literal run and
 * every length/offset is bounds-checked so a corrupt image can only fail. */
typedef struct {
	/* Next input byte value 0..255, or -1 when the source is exhausted. */
	int32_t (*in)(void *ctx);
	/* Emit one output byte at absolute position pos. */
	void (*out)(void *ctx, uint32_t pos, uint8_t byte);
	/* Read back an already emitted byte at absolute position pos - dist. */
	uint8_t (*back)(void *ctx, uint32_t pos, uint32_t dist);
	void *ctx;
} boot_lz4_io;
/* Decodes exactly in_len input bytes into at most out_cap output bytes.
 * Returns the number of output bytes produced, or -1 on any malformed,
 * truncated or overflowing stream. */
int32_t boot_lz4_decode(const boot_lz4_io *io, uint32_t in_len, uint32_t out_cap);

/* CFZ2: two byte streams (Huffman-coded literals, byte-aligned LZ4
 * structure) plus the 256-entry four-bit code-length table. Same contract:
 * returns the output length or -1 on any malformed input. */
typedef struct {
	int32_t (*in)(void *ctx);
	void *ctx;
} boot_lz4_src;
int32_t boot_lz4_decode2(const boot_lz4_io *io, const boot_lz4_src *lit,
	const boot_lz4_src *st, const uint8_t *lengths,
	uint32_t st_len, uint32_t out_cap);
#endif
