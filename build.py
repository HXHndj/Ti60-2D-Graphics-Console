"""Build the four Ti60 images and the selectable Flash image with Efinity 2026.1.

Usage: python build.py [--image all|launch|m12|key3|moonshadow] [--firmware-only]
Prerequisite: python -m pip install -r requirements.txt (Moonshadow ROM encoder).
Tool paths: --efinity HOME, --rv HOME, or EFINITY_HOME / EFINITY_RISCV_HOME.
Generated files are placed in build/. FPGA work runs in an ASCII temporary path.
Open launch/launch.xml, m12/m12.xml, key3/key3.xml or moonshadow/ddr_board.xml
in Efinity. Import each firmware/ directory into Efinity RISC-V IDE as an
existing Makefile project. Rebuild firmware before compiling FPGA after C edits.
For Flash programming use Efinity Programmer's JTAG bridge workflow with the
installed Ti60 bridge loader, then build/combined.hex. SRAM uses each .bit file.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
IMAGES = {'launch': 'launch.xml', 'm12': 'm12.xml', 'key3': 'key3.xml', 'moonshadow': 'ddr_board.xml'}

def environment(home):
    setup = home / 'bin/setup.bat'
    if not setup.is_file(): raise FileNotFoundError(setup)
    if any(c in str(setup) for c in '\r\n"%&|<>^!'):
        raise ValueError('Unsupported character in Efinity installation path')
    result = subprocess.run(f'cmd.exe /d /s /c "call "{setup}" >nul && set"', capture_output=True, check=True)
    env = {k.upper(): v for k, v in os.environ.items()}
    for line in result.stdout.decode('mbcs', errors='replace').splitlines():
        key, sep, value = line.partition('=')
        if sep and key: env[key.upper()] = value
    dirs = ['python311/bin', 'bin', 'pgm/bin', 'debugger/bin', 'scripts', 'ipm/bin/ip_packager']
    env['PATH'] = ';'.join(str(home / p) for p in dirs) + ';' + os.environ.get('PATH', '')
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONIOENCODING='utf-8', PYTHONUTF8='1', QT_PLUGIN_PATH=str(home / 'python311'))
    return env

def run(command, cwd, log, env=None):
    print('RUN', ' '.join(str(x) for x in command), flush=True)
    with log.open('wb') as stream:
        result = subprocess.run([str(x) for x in command], cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(encoding='utf-8', errors='replace')[-6000:])
        raise RuntimeError(f'Build failed ({result.returncode}); log: {log}')

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', choices=['all', *IMAGES], default='all')
    parser.add_argument('--firmware-only', action='store_true')
    parser.add_argument('--efinity', type=Path, default=os.environ.get('EFINITY_HOME', 'D:/Efinity/2026.1'))
    parser.add_argument('--rv', type=Path, default=os.environ.get('EFINITY_RISCV_HOME', 'D:/Efinity/efinity-riscv-ide-2026.1'))
    args = parser.parse_args()
    home, rv = args.efinity.resolve(), args.rv.resolve()
    images = list(IMAGES) if args.image == 'all' else [args.image]
    output = ROOT / 'build'
    output.mkdir(exist_ok=True)
    env = None if args.firmware_only else environment(home)
    with tempfile.TemporaryDirectory(prefix='ti60_submission_') as temp:
        work = Path(temp)
        if not str(work).isascii(): raise ValueError('Set TEMP to an ASCII path for Efinity')
        for image in images:
            project = work / image
            shutil.copytree(ROOT / image, project, ignore=shutil.ignore_patterns('build', 'outflow', 'work_*', 'logs', '__pycache__'))
            dest = output / image
            dest.mkdir(exist_ok=True)
            run([sys.executable, project / 'firmware/build.py', '--rv', rv], project, dest / 'firmware.log')
            shutil.copytree(project / 'firmware/build', dest / 'firmware', dirs_exist_ok=True)
            if not args.firmware_only:
                try:
                    run([home / 'python311/bin/python.exe', home / 'scripts/efx_run.py', IMAGES[image], '--flow', 'compile'], project, dest / 'compile.log', env)
                except Exception:
                    if (project / 'outflow').is_dir():
                        shutil.copytree(project / 'outflow', dest / 'failed_outflow', dirs_exist_ok=True)
                    raise
                stem = Path(IMAGES[image]).stem
                for ext in ('.bit', '.hex', '.timing.rpt', '.map.rpt'):
                    shutil.copy2(project / 'outflow' / (stem + ext), dest / (image + ext))
            print('PASS', image, flush=True)
        if args.image == 'all' and not args.firmware_only:
            # Preserve slot order: menu, BASIC/Sprite FIELD, Boss Rush, Moonshadow.
            command = [home / 'python311/bin/python.exe', home / 'pgm/bin/multi_image_merger.py', '-m', 'selectable_flash_image', '-t', 'internal']
            for image in images:
                shutil.copy2(output / image / (image + '.hex'), work / (image + '.hex'))
                command += ['-ifile', image + '.hex']
            command += ['-o', 'combined.hex']
            run(command, work, output / 'merge.log', env)
            report = (work / 'combined.rpt').read_text(errors='replace')
            import re
            layout = re.findall(r"2'b([01]{2})\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", report)
            if len(layout) != 4 or max(int(a, 16) + int(n, 16) for _, a, n in layout) > 8 * 1024 * 1024:
                raise RuntimeError('Invalid four-slot layout or W25Q64 capacity exceeded')
            for filename in ('combined.hex', 'combined.rpt'):
                shutil.copy2(work / filename, output / filename)
            print('PASS combined.hex', flush=True)

if __name__ == '__main__':
    main()
