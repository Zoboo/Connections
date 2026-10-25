#!/usr/bin/env python3
"""Package a native Connections application (program/control/CNMT NCAs).

Run make for the development NRO, or make nsp for both artifacts.
Set CONNECTIONS_HACPACK to hacPack and CONNECTIONS_NSP_KEYS to an external
console keyset. Keys are never copied into the project or printed. Installed
homebrew requires a homebrew-capable system with unsigned-content support.
Tool source: https://github.com/DarkMatterCore/hacPack (GPL-2.0).
Uses the libnx default icon and the public project RomFS; no Nintendo logo.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    # Some key parsers echo rejected lines: suppress their output completely.
    result = subprocess.run([str(arg) for arg in args], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f'{Path(str(args[0])).name} failed ({result.returncode}); '
                           'check tool/input files and external keyset')


def tool(name):
    installed = Path('C:/devkitPro/tools/bin') / (name + '.exe')
    found = shutil.which(name)
    if installed.is_file():
        return installed
    if found:
        return Path(found)
    raise RuntimeError(f'Missing devkitPro tool: {name}')


def application(titleid):
    ident = '0x' + titleid
    return {
        'name': 'Connections', 'title_id': ident,
        'title_id_range_min': ident, 'title_id_range_max': ident,
        'main_thread_stack_size': '0x100000', 'main_thread_priority': 44,
        'default_cpu_id': 0, 'process_category': 0, 'pool_partition': 0,
        'is_64_bit': True, 'address_space_type': 1, 'is_retail': True,
        # SDL graphics/audio, SD puzzle files, keyboard and wireless services.
        'filesystem_access': {'permissions': '0xffffffffffffffff',
                              'content_owner_ids': [ident]},
        'service_access': ['*'],
        'kernel_capabilities': [
            {'type': 'kernel_flags', 'value': {'highest_thread_priority': 59,
             'lowest_thread_priority': 24, 'highest_cpu_id': 2, 'lowest_cpu_id': 0}},
            {'type': 'syscalls', 'value': {f'svc{i:02x}': hex(i) for i in range(0xc0)}},
            {'type': 'application_type', 'value': 1},
            {'type': 'min_kernel_version', 'value': '0x30'},
            {'type': 'handle_table_size', 'value': 512},
            {'type': 'debug_flags', 'value': {'allow_debug': False, 'force_debug': False}},
            {'type': 'map_region', 'value': [{'region_type': 1, 'is_ro': True}]}
        ]
    }


def main():
    default_packager = shutil.which('hacpack') or ROOT / 'tools/nsp/hacpack.exe'
    if os.name != 'nt':
        default_packager = (shutil.which('hacpack') or ROOT / 'build-switch-native/tools/hacpack')
    default_keys = (Path(os.environ.get('APPDATA', '.')) / 'Ryujinx/system/prod.keys'
                    if os.name == 'nt' else Path.home() / '.switch/prod.keys')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--titleid', default='0100C04E4E530000')
    parser.add_argument('--package-revision', type=int, default=None,
                        help='Native packaging revision (0-255), independent of NRO/display version')
    parser.add_argument('--hacpack', default=os.environ.get('CONNECTIONS_HACPACK', str(default_packager)))
    parser.add_argument('--keys', default=os.environ.get('CONNECTIONS_NSP_KEYS', str(default_keys)))
    args = parser.parse_args()
    if not re.fullmatch(r'0100[0-9a-fA-F]{9}000', args.titleid):
        raise ValueError('Title ID must be a 16-digit application ID ending in 000')
    packager, keys = Path(args.hacpack).resolve(), Path(args.keys).resolve()
    if not packager.is_file():
        raise RuntimeError('Set CONNECTIONS_HACPACK to your hacPack executable')
    if not keys.is_file():
        raise RuntimeError('Set CONNECTIONS_NSP_KEYS to your external console keyset')
    stage = ROOT / 'build/nsp'
    # This fixed path is inside this project, never a caller-controlled directory.
    if stage.exists():
        shutil.rmtree(stage)
    exefs, control, ncas = [stage / name for name in ('exefs', 'control', 'ncas')]
    for directory in (exefs, control, ncas):
        directory.mkdir(parents=True)
    shutil.copyfile(ROOT / 'Connections.nso', exefs / 'main')
    config = stage / 'application.json'
    config.write_text(json.dumps(application(args.titleid), indent=2), encoding='utf-8')
    run([tool('npdmtool'), config, exefs / 'main.npdm'])
    version = re.search(r'#define CONNECTIONS_APP_VERSION "([^"]+)"',
                        (ROOT / 'source/core/version.h').read_text())[1]
    components = [int(value) for value in version.split('.')]
    if len(components) != 3 or any(value < 0 or value > 255 for value in components):
        raise ValueError('Application version must be three numbers from 0 to 255')
    if args.package_revision is None:
        args.package_revision = int(re.search(r'#define CONNECTIONS_PACKAGE_REVISION (\d+)',
            (ROOT / 'source/core/version.h').read_text())[1])
    if not 0 <= args.package_revision <= 255:
        raise ValueError('Package revision must be from 0 to 255')
    native_version = ((components[0] << 24) | (components[1] << 16) |
                      (components[2] << 8) | args.package_revision)
    run([tool('nacptool'), '--create', 'Connections', 'Zoboo', version,
         control / 'control.nacp', '--titleid=' + args.titleid])
    # Advertise only the language for which we supply an icon and localized title.
    nacp_path = control / 'control.nacp'
    nacp = bytearray(nacp_path.read_bytes())
    nacp[0x3025] = 1  # StartupUserAccount = Required (selected Switch profile)
    struct.pack_into('<I', nacp, 0x302c, 1 << 1)  # AmericanEnglish
    nacp_path.write_bytes(nacp)
    icon = Path(os.environ.get('DEVKITPRO', 'C:/devkitPro')) / 'libnx/default_icon.jpg'
    if os.name == 'nt':
        run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
             ROOT / 'packaging/prepare_icon.ps1', '-InputPath', icon,
             '-OutputPath', control / 'icon_AmericanEnglish.dat'])
    else:
        converter = shutil.which('magick')
        if not converter:
            raise RuntimeError('Install ImageMagick or supply a Windows build environment for icon conversion')
        run([converter, icon, '-resize', '256x256!', '-background', 'white',
             '-alpha', 'remove', '-alpha', 'off', '-colorspace', 'sRGB', '-type', 'TrueColor',
             '-strip', '-interlace', 'none', '-quality', '90',
             'jpg:' + str(control / 'icon_AmericanEnglish.dat')])
    default_inspector = (ROOT / 'packaging/tools/nstool/nstool.exe' if os.name == 'nt' else
                         shutil.which('nstool') or ROOT / 'build-switch-native/tools/nstool')
    inspector = Path(os.environ.get('CONNECTIONS_NSTOOL', str(default_inspector)))
    if not inspector.is_file():
        raise RuntimeError('Set CONNECTIONS_NSTOOL to the open-source nstool inspector')
    common = [packager, '-k', keys, '--titleid', args.titleid,
              '--tempdir', stage / 'temp', '--backupdir', stage / 'backup']

    def nca(kind, extra):
        before = set(ncas.glob('*.nca'))
        run(common + ['--type', 'nca', '--ncatype', kind, '--plaintext', '--outdir', ncas] + extra)
        added = set(ncas.glob('*.nca')) - before
        if len(added) != 1:
            raise RuntimeError(f'Expected one {kind} NCA')
        return added.pop()

    program = nca('program', ['--exefsdir', exefs, '--romfsdir', ROOT / 'romfs'])
    control_nca = nca('control', ['--romfsdir', control])
    meta_args = ['--titletype', 'application', '--programnca', program,
                 '--controlnca', control_nca, '--titleversion', f'{native_version:08x}']
    provisional_meta = nca('meta', meta_args)
    # hacPack defaults the CNMT digest to zero. Inspect its generated CNMT and
    # feed the real digest back through hacPack rather than patching an NCA.
    meta_extract = stage / 'provisional-meta'
    run([inspector, '-k', keys, '-x', meta_extract, provisional_meta])
    cnmt = next(path for path in meta_extract.rglob('*.cnmt') if path.is_file()).read_bytes()
    digest = hashlib.sha256(cnmt[:-32]).hexdigest()
    provisional_meta.unlink()
    nca('meta', meta_args + ['--digest', digest])
    # Plaintext auxiliary PFS0 member enables a tiny remote version check;
    # launch content remains the three normal NCAs created by hacPack.
    manifest = {'format': 1, 'title_id': args.titleid.upper(), 'version': version,
                'native_version': native_version}
    (ncas / 'connections-update.json').write_text(json.dumps(manifest), encoding='utf-8')
    run(common + ['--type', 'nsp', '--ncadir', ncas, '--outdir', stage])
    candidate = stage / (args.titleid.lower() + '.nsp')
    run([sys.executable, ROOT / 'packaging/verify_nsp.py', '--nstool', inspector,
         '--keys', keys, '--titleid', args.titleid, '--version', version,
         '--native-version', str(native_version), candidate])
    print('Verified extracted Program/Control/Meta contents, NACP StartupUserAccount=Required (1), baseline icon, NPDM IDs and CNMT hashes')
    shutil.copyfile(candidate, ROOT / 'Connections.nsp')
    # Verification has finished; keep only the installable NSP at the project root.
    shutil.rmtree(stage)
    print('Built Connections.nsp (native application: program, control and metadata NCAs)')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError) as error:
        sys.exit(str(error))
