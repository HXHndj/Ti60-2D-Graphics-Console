"""Build portable RV32 console + OCRAM loader, embed initialized image into ROM.
Copied to the exported project's firmware directory; no repository/tool skill needed.
"""
import argparse,binascii,hashlib,json,os,re,subprocess
from pathlib import Path

# ---- Compressed boot ROM container (format "CFZ1", LZ4 block payload) ----
# Layout, all little-endian 32-bit words:
#   w0 magic 0x315A4643 ("CFZ1")   w1 version 1          w2 original byte length
#   w3 original CRC32              w4 payload byte length w5 entry point 0x00100000
#   w6 CRC32 of w0..w5 bytes       w7 reserved 0
#   then the LZ4 block stream (byte-granular length w4), zero padded to a word.
# boot.c detects the magic and streams the payload through an LZ4 block
# decoder straight into DDR; images without the magic take the legacy raw
# word-copy path, so raw and compressed ROMs boot from the same loader.
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
 anchor=0;p=0;step=1
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
   p+=step;step=min(step+1,64)
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

def pack_rom_container(app):
 """app must already be padded to a 4-byte multiple (as build() does)."""
 payload=lz4_compress(app)
 header=bytearray(32)
 def put(off,val):header[off:off+4]=(val&0xFFFFFFFF).to_bytes(4,'little')
 put(0,ROM_MAGIC);put(4,ROM_VERSION);put(8,len(app))
 put(12,binascii.crc32(app)&0xFFFFFFFF);put(16,len(payload));put(20,ROM_ENTRY)
 put(24,binascii.crc32(bytes(header[:24]))&0xFFFFFFFF);put(28,0)
 rom=bytes(header)+payload
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

# ---- CFZ2: same LZ4 parsing, literals carried in a static-Huffman bitstream ----
# Structure stream (tokens, length extras, offsets) stays byte-aligned; the
# literal bytes are entropy-coded separately, so the decoder walks the
# structure and pulls N literals from the bitstream. The boot-side decoder is
# a canonical-Huffman walker: 256 four-bit lengths, no other tables.
ROM_MAGIC2=0x325A4643
ROM_VERSION2=2

def _lz4_parse(src,max_step=64,min_match=4,max_chain=16,lazy_margin=None):
 """Shared parsing core: yields (literal_bytes, match_len, match_off) ops with
 the v1 greedy decisions at max_step=64; max_step=1 probes every byte."""
 if not isinstance(max_step,int) or max_step<1:raise ValueError('max_step must be positive')
 if not isinstance(min_match,int) or min_match<4:raise ValueError('min_match must be >=4')
 if not isinstance(max_chain,int) or max_chain<1:raise ValueError('max_chain must be positive')
 n=len(src);out=[]
 HASH_BITS=15;head=[-1]*(1<<HASH_BITS);prev=[-1]*n
 def h4(p):
  return ((int.from_bytes(src[p:p+4],'little')*2654435761)>>13)&((1<<HASH_BITS)-1)
 def insert(p):
  hh=h4(p);prev[p]=head[hh];head[hh]=p
 def find_match(p):
  best=0;best_cand=-1;cand=head[h4(p)];chain=0
  while cand>=0 and chain<max_chain and p-cand<=65535:
   maxl=n-5-p
   if maxl>=4:
    l=0
    while l<maxl and src[cand+l]==src[p+l]:l+=1
    if l>best:best=l;best_cand=cand
    if best>=96:break
   cand=prev[cand];chain+=1
  return best,best_cand
 limit=n-12;anchor=0;p=0;step=1
 while p<limit:
  best,best_cand=find_match(p)
  if best>=min_match:
   # One-byte lookahead changes only token selection. Offsets still refer
   # strictly backward and the existing CFZ2 decoder is unchanged.
   if lazy_margin is not None and p+1<limit and find_match(p+1)[0]>best+lazy_margin:
    insert(p);p+=1;step=1;continue
   out.append((src[anchor:p],best,p-best_cand))
   end=p+best
   while p<end:
    insert(p);p+=1
   anchor=p;step=1
  else:
   insert(p)
   p+=step;step=min(step+1,max_step)
 out.append((src[anchor:n],0,0))
 return out

def _emit_structure(ops):
 """Token/extra/offset bytes in exactly the v1 stream layout."""
 st=bytearray()
 for lits,ml,off in ops:
  ll=len(lits)
  if ml>=4:
   st.append((min(ll,15)<<4)|min(ml-4,15))
  else:
   st.append(min(ll,15)<<4)
  e=ll-15
  if ll>=15:
   while e>=255:st.append(255);e-=255
   st.append(e)
  if ml>=4:
   st+=((off&0xFFFF).to_bytes(2,'little'))
   e=ml-19
   if ml-4>=15:
    while e>=255:st.append(255);e-=255
    st.append(e)
 return bytes(st)

def _huffman_lengths(data,maxlen=15):
 """Canonical Huffman code lengths via parent pointers, clamped to maxlen."""
 import heapq
 freq=[0]*256
 for b in data:freq[b]+=1
 live=[i for i in range(256) if freq[i]>0]
 if not live:return [0]*256
 if len(live)==1:
  lengths=[0]*256;lengths[live[0]]=1;return lengths
 h=[(freq[i],i) for i in live];heapq.heapify(h)
 parent={}
 while len(h)>1:
  f1,i1=heapq.heappop(h);f2,i2=heapq.heappop(h)
  nid=256+len(parent)
  parent[i1]=nid;parent[i2]=nid
  heapq.heappush(h,(f1+f2,nid))
 lengths=[0]*256
 for i in live:
  d=0;nod=i
  while nod in parent:nod=parent[nod];d+=1
  lengths[i]=d
 if max(lengths)>maxlen:
  # clamp (rare): flat-clamp then re-canonicalize; firmware data fits in 15
  clamped=[min(l,maxlen) for l in lengths]
  if len(live)>sum(1<<max(0,maxlen) for _ in [0]):
   pass
  # verify Kraft equality roughly; on failure raise
  kraft=sum(1<<maxlen>>l for l in clamped if l)
  if kraft>(1<<maxlen):
   raise ValueError('code lengths cannot be clamped to 15')
  lengths=clamped
 return lengths

def _canonical_first(lengths):
 """First canonical code value for each length, plus per-length populations."""
 maxlen=max(lengths)
 counts=[0]*(maxlen+2)
 for l in lengths:
  if l:counts[l]+=1
 code=0;first=[0]*(maxlen+2)
 for l in range(1,maxlen+1):
  code=(code+counts[l-1])<<1
  first[l]=code
 return first,counts,maxlen

def _encode_literals(data,lengths):
 first,counts,maxlen=_canonical_first(lengths)
 codes=[0]*256
 for sym in range(256):
  l=lengths[sym]
  if l:
   codes[sym]=(first[l],l);first[l]+=1
 bits=bytearray();acc=0;nb=0
 for b in data:
  c,l=codes[b]
  acc=(acc<<l)|c;nb+=l
  while nb>=8:
   nb-=8;bits.append((acc>>nb)&0xFF)
 if nb:
  bits.append((acc<<(8-nb))&0xFF)
 return bytes(bits)

def _decode_lit_bits(bits,total,lengths):
 """Host oracle mirroring the C canonical decode (MSB-first bit walker)."""
 first,counts,maxlen=_canonical_first(lengths)
 lits=bytearray();acc=0;nb=0;bi=0
 for _ in range(total):
  cur=0;cl=0
  while True:
   if nb==0:
    if bi>=len(bits):raise ValueError('literal bitstream exhausted')
    acc=bits[bi];bi+=1;nb=8
   nb-=1;cur=(cur<<1)|((acc>>nb)&1);cl+=1
   if cl>maxlen:raise ValueError('bad literal code')
   if counts[cl] and cur>=first[cl] and cur-first[cl]<counts[cl]:
    idx=cur-first[cl];sym=-1;seen=0
    for s2 in range(256):
     if lengths[s2]==cl:
      if seen==idx:sym=s2;break
      seen+=1
    if sym<0:raise ValueError('canonical index miss')
    lits.append(sym);break
 return bytes(lits)

def pack_rom_container2(app,max_step=64,min_match=4,max_chain=16,lazy_margin=None):
 """CFZ2: header(12B) + length table(128B) + literal bitstream + structure."""
 ops=_lz4_parse(app,max_step,min_match,max_chain,lazy_margin)
 lit=b''.join(op[0] for op in ops)
 structure=_emit_structure(ops)
 lengths=_huffman_lengths(lit)
 lbits=_encode_literals(lit,lengths)
 # the structure stream starts word-aligned so the boot reader can walk it
 lit_pad=(-len(lbits))%4
 lit_bits_padded=lbits+bytes(lit_pad)
 st_off=48+128+len(lit_bits_padded)
 header=bytearray(48)
 def put(off,val):header[off:off+4]=(val&0xFFFFFFFF).to_bytes(4,'little')
 put(0,ROM_MAGIC2);put(4,ROM_VERSION2);put(8,len(app))
 put(12,binascii.crc32(app)&0xFFFFFFFF);put(16,128+len(lit_bits_padded)+len(structure));put(20,ROM_ENTRY)
 put(28,1)  # flags: literal Huffman
 put(32,len(lbits));put(36,st_off);put(40,len(structure))
 put(24,binascii.crc32(bytes(header[:24]))&0xFFFFFFFF)
 rom=bytes(header)
 table=bytes(((lengths[2*i]<<4)|lengths[2*i+1]) for i in range(128))
 rom+=table+lit_bits_padded+structure
 return rom+bytes((-len(rom))%4)

def unpack_rom_container2(rom):
 if len(rom)<48:raise ValueError('container too small')
 m=lambda o:int.from_bytes(rom[o:o+4],'little')
 if m(0)!=ROM_MAGIC2:raise ValueError('bad magic2')
 if m(4)!=ROM_VERSION2:raise ValueError('bad version2')
 lit_bits_len=m(32);st_off=m(36)
 if binascii.crc32(rom[:24])&0xFFFFFFFF!=m(24):raise ValueError('header CRC mismatch')
 lengths=[0]*256
 for i in range(128):
  lengths[2*i]=rom[48+i]>>4;lengths[2*i+1]=rom[48+i]&15
 off=48+128
 bits=rom[off:off+lit_bits_len]
 structure=rom[st_off:st_off+m(40)]
 if len(structure)!=m(40):raise ValueError('payload truncated')
 ip=0;total_lits=0;ops=[]
 while ip<len(structure):
  tok=structure[ip];ip+=1
  ll=tok>>4;ml=(tok&15)+4
  if ll==15:
   while True:
    b=structure[ip];ip+=1;ll+=b
    if b!=255:break
  if ip>=len(structure):
   ops.append((ll,0,0));total_lits+=ll;break
  offv=structure[ip]|(structure[ip+1]<<8);ip+=2
  if (tok&15)==15:
   while True:
    b=structure[ip];ip+=1;ml+=b
    if b!=255:break
  ops.append((ll,ml,offv));total_lits+=ll
 lits=_decode_lit_bits(bits,total_lits,lengths)
 out=bytearray();li=0
 for ll,ml,offv in ops:
  out+=lits[li:li+ll];li+=ll
  if ml:
   for _ in range(ml):out.append(out[-offv])
 app=bytes(out)
 if len(app)!=m(8) or binascii.crc32(app)&0xFFFFFFFF!=m(12):raise ValueError('decoded image CRC mismatch')
 return app,{'orig_len':m(8),'payload_len':m(16),'lit_bits_len':lit_bits_len,'struct_len':m(40)}
