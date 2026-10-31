#include "gfx.h"
int gfx_text_validate(const gfx_text_frame *f){
 if(!f||f->count>16)return GFX_EINVAL;
 for(unsigned i=0;i<f->count;i++){
  uint32_t d=f->descriptors[i],x=d&1023,y=(d>>10)&1023,n=(d>>20)&127;
  if((d>>27)||y>=600||!n||n>64)return GFX_EINVAL;
  for(unsigned k=0;k<n;k++)if(f->glyphs[i][k]>63)return GFX_EINVAL;
  for(unsigned j=0;j<i;j++){
   uint32_t e=f->descriptors[j],a=e&1023,b=(e>>10)&1023,m=(e>>20)&127;
   if(x<a+m*16&&a<x+n*16&&y<b+16&&b<y+16)return GFX_EINVAL;
  }
 }
 return GFX_OK;
}
static int wait_ready(gfx_device *d,uint64_t start,uint64_t timeout){
 gfx_io *io=&d->io;
 while(!(io->read(io->ctx,GFX_REG_TEXT_STATUS)&1)){
  if(io->ticks(io->ctx)-start>=timeout)return GFX_ETIMEOUT;
  if(io->idle)io->idle(io->ctx);
 }
 return GFX_OK;
}
static int status(gfx_io *io){
 uint32_t code=(io->read(io->ctx,GFX_REG_TEXT_STATUS)>>8)&255;
 return !code?GFX_OK:(code==GFX_ERR_BUSY||code==GFX_ERR_PROTECTED)?GFX_EBUSY:GFX_EBUS;
}
int gfx_text_submit(gfx_device *d,uint32_t bank,const gfx_text_frame *f,uint64_t timeout){
 if(!d||bank>((d->caps&GFX_CAP_PRESENT_FENCE)?2u:1u)||gfx_text_validate(f))return GFX_EINVAL;
 if(!(d->caps&GFX_CAP_TEXT_OVERLAY))return GFX_EUNSUPPORTED;
 gfx_io *io=&d->io;uint64_t start=io->ticks(io->ctx);
 if(io->read(io->ctx,GFX_REG_TEXT_ID)!=0x54585431u)return GFX_EUNSUPPORTED;
 int e=wait_ready(d,start,timeout);if(e)return e;
 io->write(io->ctx,GFX_REG_TEXT_SELECT,bank);if((e=status(io)))return e;
 for(unsigned i=0;i<f->count;i++){
  unsigned n=(f->descriptors[i]>>20)&127;
  io->write(io->ctx,GFX_REG_TEXT_INDEX,i*16);
  for(unsigned j=0;j<n;j+=4){
   uint32_t word=0;
   for(unsigned k=0;k<4&&j+k<n;k++)word|=(uint32_t)f->glyphs[i][j+k]<<(8*k);
   io->write(io->ctx,GFX_REG_TEXT_DATA,word);
  }
  io->write(io->ctx,GFX_REG_TEXT_DESC_INDEX,i);
  io->write(io->ctx,GFX_REG_TEXT_DESC_DATA,f->descriptors[i]);
 }
 if(io->ticks(io->ctx)-start>=timeout)return GFX_ETIMEOUT;
 io->publish(io->ctx);io->write(io->ctx,GFX_REG_TEXT_CONTROL,1);
 if((e=status(io)))return e;
 if((e=wait_ready(d,start,timeout)))return e;
 io->publish(io->ctx);return GFX_OK;
}
