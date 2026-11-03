"""Build the submitted image corresponding to the current Flash version.

Use --rv to select the Efinity RISC-V installation. No board programming.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from rom_codec import pack_rom_container, unpack_rom_container

IMAGE = "moonshadow"
REFERENCE_APP_SHA256 = "b81fa65be8df24647a4596ee3ea4f1dc05d40a3a61e1e1aa1218b4735fa5025a"


def font_hex(source):
    text = source.read_text(encoding="utf-8")
    match = re.search(r"static const uint8_t FONT\[64\]\[8\]=(\{.*?\});", text, re.S)
    if not match:
        raise ValueError("CPU font definition missing")
    rows = json.loads(match[1].replace("{", "[").replace("}", "]"))
    if len(rows) != 64 or any(len(row) != 8 for row in rows):
        raise ValueError("Invalid font")
    return "".join(f"{value:02x}\n" for row in rows for value in row)


def build(fw, rv):
    fw = Path(fw).resolve()
    project, src, out = fw.parent, fw / "src", fw / "build"
    out.mkdir(exist_ok=True)
    tools = Path(rv) / "toolchain/bin"
    cc = tools / "riscv-none-elf-gcc.exe"
    flags = ["-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-Os", "-g", "-std=c11",
             "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
             "-ffreestanding", "-fno-builtin", "-ffunction-sections", "-fdata-sections"]
    if IMAGE == "key3":
        flags += ["-fstack-usage", "-msmall-data-limit=0", "-mno-relax", "-DMIRROR_KEY3",
                  "-DDANMAKU_DIRECT_ONLY", "-DKEY3_BATCH_FLAGS=7", "-DKEY3_SUPPLY_PROBE=1"]
        console = [p for p in sorted((src / "console").glob("*.c")) if p.name != "r6_probes.c"]
        app = [src / "app" / n for n in ["game.c", "mc_atlas.c", "mc_paint.c", "danmaku.c"]]
        linker, font = "app_key3.ld", src / "app/game.c"
    else:
        flags += ["-flto", "-msave-restore", "-msmall-data-limit=0"]
        console = sorted((src / "console").glob("*.c"))
        app = [src / "app" / n for n in ["page4/page4_font.c", "mc_atlas.c", "mc_layers.c",
                                         "mc_sprite_bins.c", "mc_paint.c"]]
        linker, font = "app.ld", src / "app/page4/page4_font.c"
    sources = [*sorted((src / "gfx").glob("*.c")), *console, *app,
               *[src / "ti60" / n for n in ["gfx_platform.c", "gfx_events.c", "console_main.c"]]]
    commands = {}

    def run(args, name):
        args = [str(x) for x in args]
        commands[name] = args
        result = subprocess.run(args, cwd=fw, capture_output=True)
        (out / (name + ".log")).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).decode(errors="replace"))

    inc = ["-I" + str(src / n) for n in ["gfx", "app", "console", "ti60"]]
    run([cc, *flags, *inc, "-nostartfiles",
         f"-Wl,--gc-sections,-T,src/ti60/{linker},-Map,build/console.map",
         src / "start.S", *sources, "-Wl,--start-group", "-lc", "-lgcc", "-lnosys",
         "-Wl,--end-group", "-o", out / "console.elf"], "app")
    run([tools / "riscv-none-elf-objcopy.exe", "-O", "binary", out / "console.elf",
         out / "console.bin"], "app_binary")
    app_bytes = (out / "console.bin").read_bytes()
    if not 0 < len(app_bytes) <= 3 * 1024 * 1024 or len(app_bytes) % 4:
        raise ValueError("Invalid application size")

    if IMAGE == "key3":
        # The accepted Flash image uses the existing CFZ1 decoder and all40
        # bits of each ROM row. DDR application bytes are unaffected.
        rom = pack_rom_container(app_bytes, compression="hc12")
        if unpack_rom_container(rom)[0] != app_bytes:
            raise ValueError("CFZ1 round-trip mismatch")
        banks = (len(rom) + 5119) // 5120
        for bank in range(banks):
            block = rom[bank * 5120:(bank + 1) * 5120]
            block += bytes(5120 - len(block))
            (project / f"console_packed40_{bank:02d}.hex").write_text("".join(
                f"{int.from_bytes(block[i:i+5], 'little'):010x}\n" for i in range(0, 5120, 5)))
        size_text = (f"`define CONSOLE_FIRMWARE_WORDS 32'd{len(rom)//4}\n"
                     f"`define CONSOLE_FIRMWARE_PACKED40_BANKS {banks}\n")
        rom_format = "CFZ1 / packed40"
        boot_flags = ["-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-Os", "-g", "-std=c11",
                      "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
                      "-ffunction-sections", "-fdata-sections", "-flto", "-msave-restore",
                      "-msmall-data-limit=0"]
        boot_limit = 2560
    else:
        # Same CFZ2 token/decoder semantics as the Flash bootstrap. Fixed ROM
        # geometry preserves the accepted94 packed5 source memories.
        from rom_codec import pack_rom_container2, unpack_rom_container2
        rom = pack_rom_container2(app_bytes, max_step=1, min_match=5,
                                  max_chain=256, lazy_margin=0)
        template = (fw / "rom_layout_template.bin").read_bytes()
        if len(rom) > len(template):
            raise ValueError("CFZ2 image exceeds the fixed Flash ROM layout")
        rom += template[len(rom):]
        if unpack_rom_container2(rom)[0] != app_bytes:
            raise ValueError("CFZ2 round-trip mismatch")
        banks = (len(rom) + 1279) // 1280
        for bank in range(banks):
            block = rom[bank * 1280:(bank + 1) * 1280]
            block += bytes(1280 - len(block))
            rows = []
            for i in range(0, 1280, 5):
                value = int.from_bytes(block[i:i+5], "little")
                rows.extend((value >> (5 * j)) & 31 for j in range(8))
            (project / f"console_packed05_{bank:03d}.hex").write_text(
                "".join(f"{v:02x}\n" for v in rows))
        size_text = (f"`define CONSOLE_FIRMWARE_WORDS 32'd{len(rom)//4}\n"
                     f"`define CONSOLE_FIRMWARE_PACKED40_BANKS {(len(rom)+5119)//5120}\n"
                     f"`define CONSOLE_FIRMWARE_PACKED05_BANKS {banks}\n")
        rom_format = "CFZ2 / packed5"
        boot_flags = ["-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-Os", "-ffreestanding",
                      "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-flto",
                      "-msave-restore", "-msmall-data-limit=0"]
        boot_limit = 2560
    if len(rom) > 131072:
        raise ValueError("ROM exceeds128 KiB")
    (out / "console_rom.bin").write_bytes(rom)
    (project / "firmware_size.vh").write_text(size_text)
    (project / "console_font.hex").write_text(font_hex(font))
    run([cc, *boot_flags, "-nostdlib", "-Wl,--gc-sections,-T,src/ocram.ld,-Map,build/boot.map",
         src / "start.S", src / "ti60/boot.c", src / "ti60/boot_lz4.c", "-lgcc",
         "-o", out / "boot.elf"], "boot")
    run([tools / "riscv-none-elf-objcopy.exe", "-O", "binary", out / "boot.elf", out / "boot.bin"],
        "boot_binary")
    boot = (out / "boot.bin").read_bytes()
    if len(boot) >= boot_limit:
        raise ValueError("Bootstrap overlaps its OCRAM stack/debug reservation")
    memory = boot + bytes(4096 - len(boot))
    for lane in range(4):
        (project / f"EfxSapphireSoc.v_toplevel_system_ramA_logic_ram_symbol{lane}.bin").write_text(
            "".join(f"{value:08b}\n" for value in memory[lane::4]))
    sha = lambda data: hashlib.sha256(data).hexdigest()
    metadata = {"image": IMAGE, "app_bytes": len(app_bytes), "app_sha256": sha(app_bytes),
                "matches_reference_application": sha(app_bytes) == REFERENCE_APP_SHA256,
                "rom_format": rom_format, "rom_bytes": len(rom), "rom_sha256": sha(rom),
                "boot_bytes": len(boot), "boot_sha256": sha(boot),
                "hardware_tested": False, "flash_written": False}
    (out / "firmware.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    print(json.dumps(metadata), flush=True)
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fw", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--rv", default=os.environ.get("EFINITY_RISCV_HOME",
                                                     "D:/Efinity/efinity-riscv-ide-2026.1"))
    args = parser.parse_args()
    build(args.fw, args.rv)
