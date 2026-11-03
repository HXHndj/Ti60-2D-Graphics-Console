#include <stdint.h>
#include "boot_lz4.h"
#define M(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define ROM 0xf8103000u
#define APP 0x00100000u
static void fail(void){M(0xf8100004)=0xbad0b007;for(;;);}
static unsigned packed40,packed10,packed5;
static uint32_t rom_cached_row=~0u,rom_cached_low,rom_cached_high;
static uint32_t rom_byte(uint32_t offset){
 uint32_t row=offset/5u,lane=offset%5u;
 if(row!=rom_cached_row){
  M(ROM)=row;__asm__ volatile("fence iorw,iorw":::"memory");
  rom_cached_low=M(ROM+4);rom_cached_high=M(ROM+12);rom_cached_row=row;
 }
 return lane==4u?rom_cached_high&255u:(rom_cached_low>>(lane*8u))&255u;
}
static uint32_t rom_word(uint32_t index){
 if(packed5){
  uint32_t bit=index*32u,row=bit/5u,shift=bit%5u,value=0u;
  for(unsigned j=0;j<8u;j++){
   if(j&&j*5u-shift>=32u)break;
   M(ROM)=row+j;__asm__ volatile("fence iorw,iorw":::"memory");
   uint32_t word=M(ROM+4)&31u;
   value|=j?word<<(j*5u-shift):word>>shift;
  }
  return value;
 }
 if(packed10){
  uint32_t bit=index*32u,row=bit/10u,shift=bit%10u,value=0u;
  for(unsigned j=0;j<4u;j++){
   M(ROM)=row+j;__asm__ volatile("fence iorw,iorw":::"memory");
   uint32_t word=M(ROM+4)&1023u;
   value|=j?word<<(j*10u-shift):word>>shift;
  }
  return value;
 }
 if(packed40){
  uint32_t value=0u;for(unsigned i=0;i<4u;i++)value|=rom_byte(index*4u+i)<<(i*8u);
  return value;
 }
 M(ROM)=index;__asm__ volatile("fence iorw,iorw":::"memory");return M(ROM+4);
}
static uint32_t crc32(const volatile uint8_t *bytes,uint32_t length){
 uint32_t crc=~0u;
 for(uint32_t index=0;index<length;index++){
  crc^=bytes[index];
  for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&-(crc&1u));
 }
 return ~crc;
}
typedef struct {uint32_t word,buffer,remaining;} rom_reader;
static int32_t rom_input(void *context){
 rom_reader *reader=context;
 if(!reader->remaining){reader->buffer=rom_word(8u+reader->word++);reader->remaining=4u;}
 int32_t byte=(int32_t)(reader->buffer&255u);
 reader->buffer>>=8;reader->remaining--;return byte;
}
static void ddr_output(void *context,uint32_t position,uint8_t byte){
 (void)context;((volatile uint8_t *)(uintptr_t)APP)[position]=byte;
}
static uint8_t ddr_previous(void *context,uint32_t position,uint32_t distance){
 (void)context;return ((volatile uint8_t *)(uintptr_t)APP)[position-distance];
}
int main(void){
 uint32_t start=M(0xf8b0bff8);
 while(!(M(0xf8100000)&1))if((uint32_t)(M(0xf8b0bff8)-start)>200000000)fail();
 uint32_t format=M(ROM+16);
 packed40=format==0x504b3430u;packed10=format==0x504b3130u;packed5=format==0x504b3035u;
 uint32_t words=M(ROM+8);if(!words||words>32768u)fail();
 uint32_t header[8],length=0u;
 if(rom_word(0)==0x315a4643u){
  if(words<9u)fail();
  for(unsigned index=0;index<8u;index++)header[index]=rom_word(index);
  length=header[2];
  if(header[1]!=1u||header[5]!=APP||header[7]||!length||length>3u*1024u*1024u||(length&3u)||
     !header[4]||header[4]>(words-8u)*4u||crc32((const volatile uint8_t *)header,24u)!=header[6])fail();
  rom_reader reader={0};
  boot_lz4_io io={rom_input,ddr_output,ddr_previous,&reader};
  if(boot_lz4_decode(&io,header[4],length)!=(int32_t)length)fail();
 }else{
  for(uint32_t index=0;index<words;index++)M(APP+index*4u)=rom_word(index);
 }
 __asm__ volatile("fence rw,rw\n.word 0x500f\nfence rw,rw":::"memory");
 if(length){
  if(crc32((const volatile uint8_t *)(uintptr_t)APP,length)!=header[3])fail();
 }else{
  for(uint32_t index=0;index<words;index++)if(M(APP+index*4u)!=rom_word(index))fail();
 }
 __asm__ volatile("fence.i":::"memory");((void (*)(void))(uintptr_t)APP)();fail();return 0;
}
