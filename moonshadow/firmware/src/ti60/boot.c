/* Fits the original 4 KiB OCRAM. Firmware ROM is included in the FPGA image.
 * The ROM image may be a raw word array (legacy) or a CFZ1 container: a
 * 32-byte header (magic/version/lengths/CRCs/entry) followed by an LZ4 block
 * stream produced by tools/console_firmware.py. Both formats boot from this
 * same loader; every header field, the decoded length and the decoded CRC32
 * are validated before the jump, and any mismatch parks in fail(). */
#include <stdint.h>
#include "boot_lz4.h"
#define M(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define ROM 0xf8103000u
#define APP 0x00100000u
#define CFZ1_MAGIC 0x315A4643u
#define CFZ1_VERSION 1u
#define CFZ2_MAGIC 0x325A4643u
#define CFZ2_VERSION 2u

static uint32_t rom_word(uint32_t index){
 uint32_t bit=index*32u,row=bit/5u,shift=bit%5u,value=0;
 for(unsigned j=0;j<8u;j++){
  if(j&&j*5u-shift>=32u)break;
  M(ROM)=row+j;__asm__ volatile("fence iorw,iorw":::"memory");
  uint32_t w=M(ROM+4)&31u;value|=j?w<<(j*5u-shift):w>>shift;
 }return value;
}
static void fail(void){M(0xf8100004)|=0x80000000u;for(;;);}

/* Bitwise CRC-32 (reflected, poly 0xEDB88320, init/xor 0xFFFFFFFF): matches
 * binascii.crc32 on the host, no table so the OCRAM budget stays small. */
static uint32_t crc32(const volatile uint8_t *p, uint32_t n){
	uint32_t c=0xFFFFFFFFu;
	for(uint32_t i=0;i<n;i++){
		c^=p[i];
		for(int k=0;k<8;k++)c=(c>>1)^(0xEDB88320u&(uint32_t)(-(int32_t)(c&1u)));
	}
	return c^0xFFFFFFFFu;
}

/* ROM payload reader: yields the LZ4 stream byte by byte. Words are fetched
 * through the indexed APB window starting at word 8 (byte offset 32, right
 * after the CFZ1 header) and unpacked little-endian, matching the packer. */
typedef struct {uint32_t word, buf, cnt;} rom_reader;
static int32_t rom_in(void *ctx){
	rom_reader *r=ctx;
	if(!r->cnt){
		r->buf=rom_word(8u+r->word);
		r->word++;r->cnt=4;
	}
	r->cnt--;
	int32_t b=(int32_t)(r->buf&0xFFu);
	r->buf>>=8;
	return b;
}
/* CFZ2 readers start at arbitrary word-aligned payload offsets */
typedef struct {rom_reader r;} rom_reader2;
static int32_t rom_in2(void *ctx){
	return rom_in(ctx);
}
static void ddr_out(void *ctx, uint32_t pos, uint8_t byte){
	(void)ctx;
	((volatile uint8_t *)(uintptr_t)APP)[pos]=byte;
}
static uint8_t ddr_back(void *ctx, uint32_t pos, uint32_t dist){
	(void)ctx;
	return ((volatile uint8_t *)(uintptr_t)APP)[pos-dist];
}

int main(void){
 M(0xf8100004)=1u; /* waiting for DDR calibration */
	uint32_t start=M(0xf8b0bff8);
	while(!(M(0xf8100000)&1))if((uint32_t)(M(0xf8b0bff8)-start)>200000000)fail();
 M(0xf8100004)=2u; /* reading the container */
	uint32_t words=M(ROM+8);if(!words||words>32768)fail();
	uint32_t h[12];
	for(uint32_t i=0;i<12;i++){h[i]=rom_word(i);}
 if(h[0]==CFZ2_MAGIC){
		/* w2 orig_len, w3 orig_crc, w4 payload bytes, w8 lit_bits_len, w9
		 * structure byte offset, w10 structure bytes; 128B length table at
		 * byte 48, literal bitstream right after, structure word-aligned */
		uint8_t lengths_tab[256];
		uint32_t li;
		if(h[1]!=CFZ2_VERSION||h[5]!=APP)fail();
		if(h[2]==0||h[2]>0x00300000u||(h[2]&3u))fail();
		if(h[4]==0||h[4]>(words-12u)*4u)fail();
		if((h[9]&3u)||h[9]<176u||h[9]+h[10]>h[4]+176u)fail();
		if(crc32((const volatile uint8_t *)h,24)!=h[6])fail();
		{rom_reader2 tp;int32_t v2b;
			tp.r.word=4u;tp.r.buf=0;tp.r.cnt=0; /* abs word 12 = payload byte 48 */
			for(li=0;li<128u;li++){
				v2b=rom_in2(&tp);
				lengths_tab[2u*li]=(uint8_t)((uint32_t)v2b>>4);
				lengths_tab[2u*li+1u]=(uint8_t)((uint32_t)v2b&15u);
			}}
		{
			rom_reader2 litp,stp;
			boot_lz4_src lit,st;
			boot_lz4_io io2;io2.in=0;io2.out=ddr_out;io2.back=ddr_back;io2.ctx=0;
			int32_t n2;
			litp.r.word=36u;litp.r.buf=0;litp.r.cnt=0; /* abs word 44 = 12-word header + 128B table */
			stp.r.word=(h[9]>>2)-8u;stp.r.buf=0;stp.r.cnt=0;
			lit.in=rom_in2;lit.ctx=&litp;
			st.in=rom_in2;st.ctx=&stp;
   M(0xf8100004)=3u; /* decoding */
			n2=boot_lz4_decode2(&io2,&lit,&st,lengths_tab,h[10],h[2]);
			if(n2<0||(uint32_t)n2!=h[2])fail();
   M(0xf8100004)=4u; /* decoded; checking DDR contents */
			__asm__ volatile("fence rw,rw\n.word 0x500f\nfence rw,rw":::"memory");
			if(crc32((const volatile uint8_t *)(uintptr_t)APP,h[2])!=h[3])fail();
   M(0xf8100004)=5u;
			__asm__ volatile("fence.i":::"memory");((void (*)(void))(uintptr_t)APP)();fail();
		}
	}
	if(h[0]==CFZ1_MAGIC){
  if(h[1]!=CFZ1_VERSION||h[5]!=APP)fail();
  if(h[2]==0||h[2]>0x00300000u||(h[2]&3u))fail();
  /* byte-granular compare; the words>=9 guard keeps (words-8)*4 from wrapping
     and payload lengths near 2^32 can no longer slip through a +3 overflow */
  if(h[4]==0||words<9u||h[4]>(words-8u)*4u)fail();
		if(crc32((const volatile uint8_t *)h,24)!=h[6])fail();
		rom_reader r={0,0,0};
		boot_lz4_io io={rom_in,ddr_out,ddr_back,&r};
  M(0xf8100004)=3u;
		int32_t n=boot_lz4_decode(&io,h[4],h[2]);
		if(n<0||(uint32_t)n!=h[2])fail();
  M(0xf8100004)=4u;
		__asm__ volatile("fence rw,rw\n.word 0x500f\nfence rw,rw":::"memory");
		if(crc32((const volatile uint8_t *)(uintptr_t)APP,h[2])!=h[3])fail();
  M(0xf8100004)=5u;
		__asm__ volatile("fence.i":::"memory");((void (*)(void))(uintptr_t)APP)();fail();
	}
 M(0xf8100004)=3u;
	for(uint32_t i=0;i<words;i++){M(APP+i*4)=rom_word(i);}
 M(0xf8100004)=4u;
	__asm__ volatile("fence rw,rw\n.word 0x500f\nfence rw,rw":::"memory");
	for(uint32_t i=0;i<words;i++){if(M(APP+i*4)!=rom_word(i))fail();}
 M(0xf8100004)=5u;
	__asm__ volatile("fence.i":::"memory");((void (*)(void))(uintptr_t)APP)();fail();return 0;
}
