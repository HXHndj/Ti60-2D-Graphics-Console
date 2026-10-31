"""Build portable RV32 console + OCRAM loader, embed initialized image into ROM.
Copied to the exported project's firmware directory; no repository/tool skill needed.
"""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
from rom_codec import pack_rom_container, unpack_rom_container

def font_hex(source):
 text=Path(source).read_text(encoding='utf-8')
 match=re.search(r'static const uint8_t FONT\[64\]\[8\]=(\{.*?\});',text,re.S)
 if not match:raise ValueError('CPU font definition missing')
 rows=json.loads(match[1].replace('{','[').replace('}',']'))
 if len(rows)!=64 or any(len(row)!=8 or any(not isinstance(x,int) or not 0<=x<=255 for x in row) for row in rows):raise ValueError('Invalid 64x8 CPU font')
 return ''.join(f'{value:02x}\n' for row in rows for value in row)

def build(fw,rv,rom_raw=False,rom_compression='greedy',rom_padding_template=None):
 fw=Path(fw).resolve();project=fw.parent;out=fw/'build';out.mkdir(exist_ok=True)
 cc=str(Path(rv)/'toolchain/bin/riscv-none-elf-gcc.exe')
 flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-g','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-flto','-msave-restore','-msmall-data-limit=0']
 def run(args,name):
  p=subprocess.run([str(x) for x in args],cwd=fw,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  (out/(name+'.log')).write_bytes(p.stdout)
  if p.returncode:raise RuntimeError(p.stdout.decode(errors='replace'))
 src=fw/'src'
 # Standalone Page 4 image: keep only the shared font/text helpers and the
 # Golden Scene atlas/layer path. Other demo page implementations are not
 # part of this firmware image.
 sources=[*sorted((src/'gfx').glob('*.c')),*sorted((src/'console').glob('*.c')),
          src/'app/page4/page4_font.c',src/'app/mc_atlas.c',src/'app/mc_layers.c',src/'app/mc_sprite_bins.c',src/'app/mc_paint.c',
          src/'ti60/gfx_platform.c',src/'ti60/gfx_events.c',src/'ti60/console_main.c']
 inc=[f'-I{src/x}' for x in ('gfx','app','console','ti60')]
 run([cc,*flags,*inc,'-nostartfiles','-Wl,--gc-sections,-T,src/ti60/app.ld,-Map,build/console.map',src/'start.S',*sources,'-Wl,--start-group','-lc','-lgcc','-lnosys','-Wl,--end-group','-o',out/'console.elf'],'app')
 run([Path(rv)/'toolchain/bin/riscv-none-elf-objcopy.exe','-O','binary',out/'console.elf',out/'console.bin'],'app_binary')
 data=(out/'console.bin').read_bytes();data+=bytes((-len(data))%4)
 if not 0<len(data)<=3*1024*1024:raise ValueError('Application exceeds 3 MiB DDR slot')
 if rom_raw and (rom_compression!='greedy' or rom_padding_template):
  raise ValueError('Raw ROM cannot use compressed ROM layout options')
 template=Path(rom_padding_template).read_bytes() if rom_padding_template else None
 rom=data if rom_raw else pack_rom_container(data,compression=rom_compression,padding_template=template)
 if len(rom)>131072:raise ValueError('Compressed ROM exceeds 128 KiB hardware capacity')
 if not rom_raw and unpack_rom_container(rom)[0]!=data:raise ValueError('ROM compression round trip failed')
 (out/'console_rom.bin').write_bytes(rom)
 (project/'console_firmware.hex').write_text(''.join(f'{int.from_bytes(rom[i:i+4],"little"):08x}\n' for i in range(0,len(rom),4)))
 bank_count=(len(rom)+4095)//4096
 for old in project.glob('console_firmware_[0-9][0-9].hex'):
  if int(old.stem.rsplit('_',1)[1])>=bank_count:old.unlink()
 for bank in range(bank_count):
  block=rom[bank*4096:(bank+1)*4096];block+=bytes(4096-len(block))
  (project/f'console_firmware_{bank:02d}.hex').write_text(''.join(f'{int.from_bytes(block[i:i+4],"little"):08x}\n' for i in range(0,4096,4)))
 # Pack five ROM bytes per 40-bit row, using all four RAM10 lanes.
 # Preserve the 32-bit files for compatible existing carrier bitstreams.
 packed_banks=(len(rom)+5119)//5120
 for bank in range(packed_banks):
  block=rom[bank*5120:(bank+1)*5120];block+=bytes(5120-len(block))
  (project/f'console_packed40_{bank:02d}.hex').write_text(''.join(f'{int.from_bytes(block[i:i+5],"little"):010x}\n' for i in range(0,5120,5)))
 # The same packed bitstream, narrow output: fewer LUTs in bank selection.
 for bank in range(packed_banks):
  block=rom[bank*5120:(bank+1)*5120];block+=bytes(5120-len(block))
  words=[]
  for i in range(0,5120,5):
   group=int.from_bytes(block[i:i+5],"little")
   words.extend((group>>(10*j))&1023 for j in range(4))
  (project/f'console_packed10_{bank:02d}.hex').write_text(''.join(f'{word:03x}\n' for word in words))
 packed5_banks=(len(rom)+1279)//1280
 for bank in range(packed5_banks):
  block=rom[bank*1280:(bank+1)*1280];block+=bytes(1280-len(block))
  words=[]
  for i in range(0,1280,5):
   group=int.from_bytes(block[i:i+5],"little")
   words.extend((group>>(5*j))&31 for j in range(8))
  (project/f'console_packed05_{bank:03d}.hex').write_text(''.join(f'{word:02x}\n' for word in words))
 (project/'firmware_size.vh').write_text(f"`define CONSOLE_FIRMWARE_WORDS 32'd{len(rom)//4}\n`define CONSOLE_FIRMWARE_PACKED40_BANKS {packed_banks}\n`define CONSOLE_FIRMWARE_PACKED05_BANKS {packed5_banks}\n")
 (project/'console_font.hex').write_text(font_hex(src/'app/page4/page4_font.c'))
 run([cc,*flags,'-nostdlib','-Wl,--gc-sections,-T,src/ocram.ld,-Map,build/boot.map',src/'start.S',src/'ti60/boot.c',src/'ti60/boot_lz4.c','-lgcc','-o',out/'boot.elf'],'boot')
 run([Path(rv)/'toolchain/bin/riscv-none-elf-objcopy.exe','-O','binary',out/'boot.elf',out/'boot.bin'],'boot_binary')
 boot=(out/'boot.bin').read_bytes();assert len(boot)<2560;mem=boot+bytes(4096-len(boot))
 for lane in range(4):(project/f'EfxSapphireSoc.v_toplevel_system_ramA_logic_ram_symbol{lane}.bin').write_text(''.join(f'{b:08b}\n' for b in mem[lane::4]))
 run([Path(rv)/'toolchain/bin/riscv-none-elf-size.exe',out/'console.elf',out/'boot.elf'],'size')
 result={'rom_bytes':len(rom),'rom_banks':bank_count,'packed05_banks':packed5_banks,'packed40_banks':packed_banks,'packed40_expected_ram10':packed_banks*4,'rom_format':'raw' if rom_raw else 'lz4_cfz1','rom_sha256':hashlib.sha256(rom).hexdigest(),'app_bytes':len(data),'boot_bytes':len(boot),'app_sha256':hashlib.sha256(data).hexdigest(),'boot_sha256':hashlib.sha256(boot).hexdigest(),'font_sha256':hashlib.sha256((project/'console_font.hex').read_bytes()).hexdigest(),'hardware_tested':False}
 result.update(rom_compression=rom_compression,rom_padding_template_sha256=hashlib.sha256(template).hexdigest() if template is not None else None)
 (out/'firmware.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));return result

def sync_repo_export(script):
 """Repository mode only: refresh the exported firmware tree from sw/ using
 the exact console_project.py export mapping (sw/gfx|app|console|platform/ti60
 -> src/..., board_smoke start.S+ocram.ld, and this script as build.py) so the
 built ROM can never drift from the reviewed sources. Relocated copies of this
 script (firmware/build.py inside an export) have no adjacent sw/ and are
 left untouched, preserving the frozen-export build behaviour."""
 import shutil
 root=script.parents[1]
 fw=root/'projects/console/firmware'
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
 if (here/'src').exists():
  fw_default=here
 else:
  fw_default=here.parent/'projects/console/firmware'
  sync_repo_export(Path(__file__).resolve())
 p=argparse.ArgumentParser();p.add_argument('--fw',type=Path,default=fw_default);p.add_argument('--rv',default=os.environ.get('EFINITY_RISCV_HOME','D:/Efinity/efinity-riscv-ide-2026.1'));p.add_argument('--rom-raw',action='store_true');p.add_argument('--rom-compression',choices=('greedy','hc12'),default='hc12');p.add_argument('--rom-padding-template',type=Path,default=here/'rom_layout_template.bin');a=p.parse_args();build(a.fw,a.rv,a.rom_raw,a.rom_compression,a.rom_padding_template)
