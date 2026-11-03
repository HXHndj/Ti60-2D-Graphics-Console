import binascii

ROM_MAGIC=0x315A4643
ROM_VERSION=1
ROM_ENTRY=0x00100000

def lz4_compress(src):
 """Deterministic LZ4 block encoder (spec-compatible: last 5 bytes are
 literals, no match starts within the last 12 bytes, offsets are 16-bit).
 Greedy longest-match with a small hash chain; only ever references already
 emitted output, so the byte-wise decoder in boot.c is an exact inverse."""
 n=len(src);out=bytearray()
 HASH_BITS=15;head=[-1]*(1<<HASH_BITS);prev=[-1]*n
 def h4(p):
  return ((int.from_bytes(src[p:p+4],'little')*2654435761)>>13)&((1<<HASH_BITS)-1)
 def insert(p):
  h=h4(p);prev[p]=head[h];head[h]=p
 limit=n-12  # conservative: no match starts at/after n-12
 anchor=0;p=0
 while p<limit:
  best=0;best_cand=-1;cand=head[h4(p)];chain=0
  while cand>=0 and chain<16 and p-cand<=65535:
   maxl=n-5-p  # matches never cover the last 5 literal bytes
   if maxl>=4:
    l=0
    while l<maxl and src[cand+l]==src[p+l]:l+=1
    if l>best:best=l;best_cand=cand
    if best>=96:break
   cand=prev[cand];chain+=1
  if best>=4:
   ll=p-anchor;ml=best
   out.append((min(ll,15)<<4)|min(ml-4,15))
   e=ll-15
   if ll>=15:
    while e>=255:out.append(255);e-=255
    out.append(e)
   out+=src[anchor:p]
   out+=((p-best_cand)&0xFFFF).to_bytes(2,'little')
   e=ml-19  # (ml-4)-15
   if ml-4>=15:
    while e>=255:out.append(255);e-=255
    out.append(e)
   end=p+ml
   while p<end:
    insert(p);p+=1
   anchor=p;step=1
  else:
   insert(p)
   # Insert every position so later companion/trajectory code remains highly
   # compressible; the decoder format and deterministic output are unchanged.
   p+=1
 ll=n-anchor
 out.append(min(ll,15)<<4)
 e=ll-15
 if ll>=15:
  while e>=255:out.append(255);e-=255
  out.append(e)
 out+=src[anchor:n]
 return bytes(out)

def lz4_decompress(blk,out_cap):
 """Host oracle decoder: mirrors boot.c byte-wise semantics exactly.
 Returns the decoded bytes; raises on any malformed/truncated input."""
 ip=0;n=len(blk);out=bytearray()
 while ip<n:
  tok=blk[ip];ip+=1
  ll=tok>>4
  if ll==15:
   while True:
    if ip>=n:raise ValueError('literal length overruns input')
    b=blk[ip];ip+=1;ll+=b
    if b!=255:break
  if ip+ll>n or len(out)+ll>out_cap:raise ValueError('literal run overruns input/output')
  out+=blk[ip:ip+ll];ip+=ll
  if ip==n:break
  if ip+2>n:raise ValueError('offset overruns input')
  off=blk[ip]|blk[ip+1]<<8;ip+=2
  if off==0 or off>len(out):raise ValueError('bad match offset')
  ml=(tok&15)+4
  if (tok&15)==15:
   while True:
    if ip>=n:raise ValueError('match length overruns input')
    b=blk[ip];ip+=1;ml+=b
    if b!=255:break
  if len(out)+ml>out_cap:raise ValueError('match overruns output')
  for _ in range(ml):out.append(out[-off])
 if ip!=n:raise ValueError('trailing bytes')
 return bytes(out)

def pack_rom_container(app,compression='greedy',padding_template=None):
 """app must already be padded to a 4-byte multiple (as build() does)."""
 if compression=='greedy':payload=lz4_compress(app)
 elif compression=='hc12':
  import lz4.block
  payload=lz4.block.compress(app,mode='high_compression',compression=12,store_size=False)
 else:raise ValueError('Unknown ROM compression: '+compression)
 header=bytearray(32)
 def put(off,val):header[off:off+4]=(val&0xFFFFFFFF).to_bytes(4,'little')
 put(0,ROM_MAGIC);put(4,ROM_VERSION);put(8,len(app))
 put(12,binascii.crc32(app)&0xFFFFFFFF);put(16,len(payload));put(20,ROM_ENTRY)
 put(24,binascii.crc32(bytes(header[:24]))&0xFFFFFFFF);put(28,0)
 rom=bytes(header)+payload
 if padding_template is not None:
  if len(padding_template)%4 or len(rom)>len(padding_template):
   raise ValueError('Compressed ROM exceeds aligned padding template capacity')
  return rom+padding_template[len(rom):]
 return rom+bytes((-len(rom))%4)

def unpack_rom_container(rom):
 """Host-side verification: returns (app_bytes, meta) or raises."""
 if len(rom)<32:raise ValueError('container too small')
 m=lambda o:int.from_bytes(rom[o:o+4],'little')
 if m(0)!=ROM_MAGIC:raise ValueError('bad magic')
 if m(4)!=ROM_VERSION:raise ValueError('bad version')
 meta={'orig_len':m(8),'orig_crc32':m(12),'payload_len':m(16),'entry':m(20),'header_crc32':m(24)}
 if binascii.crc32(rom[:24])&0xFFFFFFFF!=meta['header_crc32']:raise ValueError('header CRC mismatch')
 payload=rom[32:32+meta['payload_len']]
 if len(payload)!=meta['payload_len']:raise ValueError('payload truncated')
 app=lz4_decompress(payload,meta['orig_len'])
 if len(app)!=meta['orig_len'] or binascii.crc32(app)&0xFFFFFFFF!=meta['orig_crc32']:
  raise ValueError('decoded image CRC mismatch')
 return app,meta
