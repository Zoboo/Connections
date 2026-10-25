#!/usr/bin/env python3
"""Verify a built NSP by extracting it with nstool before publishing it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect(tool, keys, path, output):
    result = subprocess.run([str(tool), '-k', str(keys), '-y', '-x', str(output), str(path)],
                            capture_output=True, text=True)
    require(result.returncode == 0, f'nstool could not inspect {path.name}')
    text = result.stdout + result.stderr
    for line in text.splitlines():
        if '[WARNING]' in line or 'ERROR' in line:
            # Unsigned homebrew signatures are expected. nstool 1.9.2 cannot
            # decode the valid newer MapRegion capability; validate KAC below.
            expected = ('NCA Header Main Signature: FAIL' in line or
                        'NCA Header ACID Signature: FAIL' in line or
                        'keyfile' in line.lower())
            require(expected, f'nstool reported an unexpected validation warning for {path.name}')
    return text


def baseline_jpeg(data):
    require(data[:2] == b'\xff\xd8' and data[-2:] == b'\xff\xd9', 'Icon is not a complete JPEG')
    offset = 2
    found = False
    while offset < len(data):
        require(data[offset] == 0xff, 'Invalid JPEG marker')
        while data[offset] == 0xff:
            offset += 1
        marker = data[offset]
        offset += 1
        if marker == 0xda:  # Start of baseline scan
            break
        size = int.from_bytes(data[offset:offset+2], 'big')
        require(size >= 2 and offset+size <= len(data), 'Invalid JPEG segment size')
        if marker in (0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf):
            require(marker == 0xc0, 'HOME icon must be baseline JPEG, not progressive')
            frame = data[offset+2:offset+size]
            require(frame[:6] == b'\x08\x01\x00\x01\x00\x03', 'Icon must be 8-bit, 256x256, three-component JPEG')
            found = True
        offset += size
    require(found, 'No baseline JPEG frame in icon')


def verify_npdm(data, title):
    require(data[:4] == b'META' and len(data) >= 0x80, 'Invalid NPDM META header')
    require(data[0xc] & 1 and (data[0xc] >> 1) & 7 == 1, 'NPDM must be AArch64 with application address space')
    aci, aci_size, acid, acid_size = struct.unpack_from('<IIII', data, 0x70)
    require(aci+aci_size <= len(data) and acid+acid_size <= len(data), 'Invalid NPDM bounds')
    require(data[aci:aci+4] == b'ACI0' and data[acid+0x200:acid+0x204] == b'ACID', 'Invalid NPDM access descriptors')
    require(struct.unpack_from('<Q', data, aci+0x10)[0] == title, 'NPDM program ID mismatch')
    require(struct.unpack_from('<QQ', data, acid+0x210) == (title, title), 'NPDM title range mismatch')
    all_caps = []
    for base, size, field in ((aci, aci_size, 0x30), (acid, acid_size, 0x230)):
        offset, length = struct.unpack_from('<II', data, base+field)
        require(length % 4 == 0 and offset+length <= size, 'Invalid NPDM capability bounds')
        caps = [v[0] for v in struct.iter_unpack('<I', data[base+offset:base+offset+length])]
        for value in caps:
            trailing_ones = 0
            while trailing_ones < 32 and value & (1 << trailing_ones):
                trailing_ones += 1
            require(trailing_ones in (3,4,6,7,10,11,13,14,15,16,32), 'Unknown NPDM kernel capability')
        require(any(v & 0x3fff == 0x1fff and (v >> 14) & 7 == 1 for v in caps), 'NPDM is not Application type')
        all_caps.append(caps)
    require(all_caps[0] == all_caps[1], 'ACI0/ACID capability mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nstool', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--titleid', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--native-version', type=int, required=True)
    parser.add_argument('nsp', type=Path)
    args = parser.parse_args()
    title = int(args.titleid, 16)
    output = args.nsp.parent / 'verified'
    output.mkdir()
    inspect(args.nstool, args.keys, args.nsp, output)
    manifest = json.loads((output / 'connections-update.json').read_text(encoding='utf-8'))
    require(manifest == {'format': 1, 'title_id': args.titleid.upper(), 'version': args.version,
                         'native_version': args.native_version}, 'NSP update manifest mismatch')
    contents = {}
    ncas = list(output.glob('*.nca'))
    require(len(ncas) == 3, 'NSP must contain exactly Program, Control and Meta NCAs')
    for path in ncas:
        extracted = output / (path.name + '-contents')
        text = inspect(args.nstool, args.keys, path, extracted)
        kind = next((kind for kind in ('Program', 'Control', 'Meta') if f'Content Type:    {kind}' in text), None)
        require(kind and kind not in contents, 'Missing or duplicate NCA content type')
        require(f'ProgID:          0x{title:016x}' in text, 'NCA title ID mismatch')
        require('Content Index:   0' in text, 'Unexpected NCA content index')
        digest = hashlib.sha256(path.read_bytes()).digest()
        require(path.name.split('.')[0] == digest[:16].hex(), 'NCA filename/content ID mismatch')
        contents[kind] = (path, extracted, digest)
    program = contents['Program'][1]
    main_nso = (program / '0/main').read_bytes()
    require(main_nso[:4] == b'NSO0', 'Program is not a native NSO executable')
    require(main_nso == (Path(__file__).resolve().parents[1] / 'Connections.nso').read_bytes(), 'Packed executable differs from built NSO')
    verify_npdm((program / '0/main.npdm').read_bytes(), title)
    control = contents['Control'][1] / '0'
    nacp = (control / 'control.nacp').read_bytes()
    require(len(nacp) == 0x4000, 'Invalid NACP size')
    require(nacp[0x3025] == 1, 'StartupUserAccount must be Required (1) in final Control NCA')
    require(nacp[0x300:0x500].split(b'\0')[0] == b'Connections', 'Application name mismatch')
    require(nacp[0x500:0x600].split(b'\0')[0] == b'Zoboo', 'Publisher mismatch')
    require(nacp[0x3060:0x3070].split(b'\0')[0].decode() == args.version, 'Display version mismatch')
    require(struct.unpack_from('<I', nacp, 0x302c)[0] == 2, 'Icon/language mask mismatch')
    for offset in (0x3038, 0x3078, 0x30b0):
        require(struct.unpack_from('<Q', nacp, offset)[0] == title, 'NACP title ID mismatch')
    baseline_jpeg((control / 'icon_AmericanEnglish.dat').read_bytes())
    cnmt_path = next(p for p in contents['Meta'][1].rglob('*.cnmt') if p.is_file())
    cnmt = cnmt_path.read_bytes()
    require(struct.unpack_from('<Q', cnmt)[0] == title and cnmt[12] == 0x80, 'CNMT must describe this base application')
    require(struct.unpack_from('<I', cnmt, 8)[0] == args.native_version, 'CNMT version mismatch')
    require(struct.unpack_from('<HHH', cnmt, 14) == (16,2,0), 'Unexpected CNMT record layout')
    require(struct.unpack_from('<Q', cnmt, 0x20)[0] == title + 0x800, 'Unexpected application patch ID')
    require(struct.unpack_from('<Q', cnmt, 32)[0] == title+0x800, 'Invalid CNMT patch ID')
    require(hashlib.sha256(cnmt[:-32]).digest() == cnmt[-32:], 'Invalid CNMT digest')
    for index, kind in enumerate(('Program','Control')):
        record = cnmt[48+index*56:104+index*56]
        path, _, digest = contents[kind]
        require(record[:32] == digest and record[32:48] == digest[:16], 'CNMT content hash/ID mismatch')
        require(int.from_bytes(record[48:54], 'little') == path.stat().st_size, 'CNMT content size mismatch')
        require(record[54:] == bytes((1 if kind == 'Program' else 3, 0)), 'CNMT content type/index mismatch')
    print('Verified NSP: native Program/NPDM, Control/NACP/baseline icon, Meta/CNMT hashes and consistent title IDs')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, StopIteration) as error:
        sys.exit(f'NSP verification failed: {error}')
