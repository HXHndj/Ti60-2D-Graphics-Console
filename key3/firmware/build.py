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

def font_hex(source):
 text=Path(source).read_text(encoding='utf-8')
 match=re.search(r'static const uint8_t FONT\[64\]\[8\]=(\{.*?\});',text,re.S)
 if not match:raise ValueError('CPU font definition missing')
 rows=json.loads(match[1].replace('{','[').replace('}',']'))
 if len(rows)!=64 or any(len(row)!=8 or any(not isinstance(x,int) or not 0<=x<=255 for x in row) for row in rows):raise ValueError('Invalid 64x8 CPU font')
 return ''.join(f'{value:02x}\n' for row in rows for value in row)

def build(fw,rv,rom_raw=False,mirror='full',key3_batch_flags=7,key3_supply_probe=1):
 fw=Path(fw).resolve();project=fw.parent;out=fw/'build';out.mkdir(exist_ok=True)
 cc=str(Path(rv)/'toolchain/bin/riscv-none-elf-gcc.exe')
 flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-g','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-msmall-data-limit=0','-mno-relax']
 if mirror=='m12':flags+=['-DMIRROR_M12']
 elif mirror=='key3':flags+=['-DMIRROR_KEY3','-DDANMAKU_DIRECT_ONLY',f'-DKEY3_BATCH_FLAGS={key3_batch_flags}',f'-DKEY3_SUPPLY_PROBE={key3_supply_probe}']
 elif mirror=='launch':flags+=['-DMIRROR_LAUNCH']
 def run(args,name):
  p=subprocess.run([str(x) for x in args],cwd=fw,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  (out/(name+'.log')).write_bytes(p.stdout)
  if p.returncode:raise RuntimeError(p.stdout.decode(errors='replace'))
 src=fw/'src'
 # Mirror source sets (2026-10-21 split): full = regression with key1/2/3,
 # m12 = BASIC+COMPARE, key3 = EXTENSION game. mc_layers/mc_runner/scene are
 # retired everywhere (tile4 removal / zero references).
 console_c=[c for c in sorted((src/'console').glob('*.c')) if not (mirror in ('key3','launch') and c.name=='r6_probes.c')]
 app_common=[src/'app/game.c',src/'app/mc_atlas.c',src/'app/mc_paint.c']
 if mirror=='m12':app_scenes=[src/'app/stress.c',src/'app/stress_bucket.c']
 elif mirror=='key3':app_scenes=[src/'app/danmaku.c']
 elif mirror=='launch':app_scenes=[]
 else:app_scenes=[src/'app/danmaku.c',src/'app/stress.c',src/'app/stress_bucket.c']
 sources=[*sorted((src/'gfx').glob('*.c')),*console_c,*app_common,*app_scenes,src/'ti60/gfx_platform.c',src/'ti60/gfx_events.c',src/'ti60/console_main.c']
 inc=[f'-I{src/x}' for x in ('gfx','app','console','ti60')]
 linker='app_key3.ld' if mirror=='key3' else 'app.ld'
 run([cc,*flags,*inc,'-nostartfiles',f'-Wl,--gc-sections,-T,src/ti60/{linker},-Map,build/console.map',src/'start.S',*sources,'-Wl,--start-group','-lc','-lgcc','-lnosys','-Wl,--end-group','-o',out/'console.elf'],'app')
 run([Path(rv)/'toolchain/bin/riscv-none-elf-objcopy.exe','-O','binary',out/'console.elf',out/'console.bin'],'app_binary')
 data=(out/'console.bin').read_bytes();data+=bytes((-len(data))%4)
 # The application executes in the existing 3 MiB DDR APP region; app.ld
 # separately checks BSS and stack. The 15-bit ROM index limits the encoded
 # container, not the decompressed DDR image (boot.c accepts up to 3 MiB).
 if not 0<len(data)<=0x300000:raise ValueError('Application exceeds DDR APP region')
 if rom_raw:
  rom=data;rom_meta={'rom_format':'raw','rom_bytes':len(rom)}
 else:
  rom1=pack_rom_container(data);rom2=pack_rom_container2(data)
  # Dense scanning is a build-time compression choice, not an application
  # or decoder change. Keep the sparse candidate if dense scanning loses.
  dense=pack_rom_container2(data,max_step=1)
  parse_step=64;parse_match=4;parse_chain=16;parse_lazy=None
  if len(dense)<len(rom2):rom2=dense;parse_step=1
  # CFZ2 entropy-codes literals separately: a four-byte match can cost more
  # structure bytes than its encoded literals. Try omitting these matches.
  # Decoder/token format and application/loader bytes stay unchanged.
  dense5=pack_rom_container2(data,max_step=1,min_match=5)
  if len(dense5)<len(rom2):rom2=dense5;parse_step=1;parse_match=5
  # Build-time only: try deeper match search without changing the decoder.
  # Physical RAM width/allocation is accepted only after real mapping.
  for chain_limit in (32,64,128,256):
   candidate=pack_rom_container2(data,max_step=1,min_match=5,max_chain=chain_limit)
   if len(candidate)<len(rom2):
    rom2=candidate;parse_step=1;parse_match=5;parse_chain=chain_limit
  lazy=pack_rom_container2(data,max_step=1,min_match=5,max_chain=256,lazy_margin=0)
  if len(lazy)<len(rom2):
   rom2=lazy;parse_step=1;parse_match=5;parse_chain=256;parse_lazy=0
  if len(rom2)<len(rom1):
   app2,meta=unpack_rom_container2(rom2)
   if app2!=data:raise ValueError('CFZ2 round-trip mismatch')
   rom=rom2;rom_meta={'rom_format':'lz4_cfz2','rom_bytes':len(rom),'rom_parse_max_step':parse_step,'rom_parse_min_match':parse_match,'rom_parse_max_chain':parse_chain,'rom_parse_lazy_margin':parse_lazy,**meta}
  else:
   app2,meta=unpack_rom_container(rom1)
   if app2!=data:raise ValueError('CFZ1 round-trip mismatch')
   rom=rom1;rom_meta={'rom_format':'lz4_cfz1','rom_bytes':len(rom),**meta}
 if not 0<len(rom)<=131072:raise ValueError('Encoded firmware exceeds 128 KiB ROM')
 (project/'console_firmware.hex').write_text(''.join(f'{int.from_bytes(rom[i:i+4],"little"):08x}\n' for i in range(0,len(rom),4)))
 for bank in range((len(rom)+4095)//4096):
  block=rom[bank*4096:(bank+1)*4096];block+=bytes(4096-len(block))
  (project/f'console_firmware_{bank:02d}.hex').write_text(''.join(f'{int.from_bytes(block[i:i+4],"little"):08x}\n' for i in range(0,4096,4)))
 # a shorter image (e.g. compressed) must not leave stale higher banks behind
 for stale in project.glob('console_firmware_[0-9][0-9].hex'):
  try:idx=int(stale.name[len('console_firmware_'):-len('.hex')])
  except ValueError:continue
  if idx>=(len(rom)+4095)//4096:stale.unlink()
 (project/'firmware_size.vh').write_text(f"`define CONSOLE_FIRMWARE_WORDS 32'd{len(rom)//4}\n")
 (project/'console_font.hex').write_text(font_hex(src/'app/game.c'))
 run([cc,*flags,'-fno-builtin','-fno-tree-loop-distribute-patterns','-nostdlib','-Wl,--gc-sections,-T,src/ocram.ld,-Map,build/boot.map',src/'start.S',src/'ti60/boot.c',src/'ti60/boot_lz4.c','-lgcc','-o',out/'boot.elf'],'boot')
 run([Path(rv)/'toolchain/bin/riscv-none-elf-objcopy.exe','-O','binary',out/'boot.elf',out/'boot.bin'],'boot_binary')
 boot=(out/'boot.bin').read_bytes();assert len(boot)<3584;mem=boot+bytes(4096-len(boot))
 for lane in range(4):(project/f'EfxSapphireSoc.v_toplevel_system_ramA_logic_ram_symbol{lane}.bin').write_text(''.join(f'{b:08b}\n' for b in mem[lane::4]))
 run([Path(rv)/'toolchain/bin/riscv-none-elf-size.exe',out/'console.elf',out/'boot.elf'],'size')
 result={'mirror':mirror,'key3_direct_only':mirror=='key3','key3_batch_flags':key3_batch_flags if mirror=='key3' else None,'key3_supply_probe':key3_supply_probe if mirror=='key3' else None,'app_bytes':len(data),'boot_bytes':len(boot),'app_sha256':hashlib.sha256(data).hexdigest(),'boot_sha256':hashlib.sha256(boot).hexdigest(),
  'rom_sha256':hashlib.sha256(rom).hexdigest(),'rom_words':len(rom)//4,'rom_banks':(len(rom)+4095)//4096,**rom_meta,
  'font_sha256':hashlib.sha256((project/'console_font.hex').read_bytes()).hexdigest(),'hardware_tested':False}
 (out/'firmware.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));return result

def sync_repo_export(script,mirror='full'):
 """Repository mode only: refresh the exported firmware tree from sw/ using
 the exact console_project.py export mapping (sw/gfx|app|console|platform/ti60
 -> src/..., board_smoke start.S+ocram.ld, and this script as build.py) so the
 built ROM can never drift from the reviewed sources. Relocated copies of this
 script (firmware/build.py inside an export) have no adjacent sw/ and are
 left untouched, preserving the frozen-export build behaviour."""
 import shutil
 root=script.parents[1]
 project_dir='console' if mirror=='full' else mirror
 fw=root/'projects'/project_dir/'firmware'
 if not (root/'sw'/'gfx').is_dir() or not fw.is_dir():return
 src=fw/'src'
 shutil.rmtree(src,ignore_errors=True);src.mkdir(parents=True)
 for folder,target in [('sw/gfx','gfx'),('sw/app','app'),('sw/console','console'),('sw/platform/ti60','ti60')]:
  shutil.copytree(root/folder,src/target)
 for name in ['start.S','ocram.ld']:
  shutil.copy2(root/'sw/board_smoke'/name,src/name)
 shutil.copy2(script,fw/'build.py')

if __name__=='__main__':
 here=Path(__file__).resolve().parent
 # Beside an exported src/ tree (firmware/build.py copies) keep the old
 # default; from the repository tools/ dir fall back to the exported project
 # and re-sync it from sw/ first (see sync_repo_export).
 project_dir_arg=None
 import sys
 pre=argparse.ArgumentParser(add_help=False)
 pre.add_argument('--mirror',default='key3');pre.add_argument('--fw',type=Path,default=None)
 known,_=pre.parse_known_args()
 mirror_sel=known.mirror if known.mirror in ('full','m12','key3','launch') else 'key3'
 project_dir='console' if mirror_sel=='full' else mirror_sel
 if (here/'src').exists():
  fw_default=here
 else:
  fw_default=here.parent/'projects'/project_dir/'firmware'
  if known.fw is None:
   import shutil
   fw_default.parent.mkdir(parents=True,exist_ok=True)
   if not fw_default.is_dir():
    (fw_default/'src').mkdir(parents=True)
  sync_repo_export(Path(__file__).resolve(),mirror_sel)
 p=argparse.ArgumentParser();p.add_argument('--fw',type=Path,default=fw_default);p.add_argument('--rv',default=os.environ.get('EFINITY_RISCV_HOME','D:/Efinity/efinity-riscv-ide-2026.1'));p.add_argument('--rom-raw',action='store_true',default=bool(os.environ.get('CONSOLE_ROM_RAW')),help='embed the raw image instead of the CFZ1/LZ4 container');p.add_argument('--mirror',choices=('full','m12','key3','launch'),default=mirror_sel,help='firmware image selection (pages and scene sources)');p.add_argument('--key3-batch-flags',type=int,choices=range(8),default=7);p.add_argument('--key3-supply-probe',type=int,choices=(0,1),default=1);a=p.parse_args();build(a.fw,a.rv,rom_raw=a.rom_raw,mirror=a.mirror,key3_batch_flags=a.key3_batch_flags,key3_supply_probe=a.key3_supply_probe)
