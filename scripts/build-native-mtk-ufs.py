#!/usr/bin/env python3
"""Compile the yunluo UFS archive against an existing, matching XNU recipe.

Derived from UniXNU's build-arm-driver.py. This only builds the component;
it does not prepare XNU, download headers, link a kernel or access a device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

COMPONENT = 'NativeMtkUfs'
REPO = Path(__file__).resolve().parents[1]


def compile_command(record, source, destination, tree):
    """Replay the target ABI while redirecting all compiler output files."""
    arguments = record['arguments']
    command = []
    skip_value = {'-o', '-MF', '-MT', '-MQ', '-MJ', '-dependency-file', '-serialize-diagnostics'}
    index = 0
    replaced = False
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            replaced = True
        elif value in skip_value:
            index += 1
        elif value in ('-MD', '-MMD', '-MP', '-MG'):
            pass
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise ValueError('recipe file is not present in compiler arguments')
    command += ['-o', str(destination), '-Wall', '-Wextra', '-Werror',
                '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
                '-Wno-macro-redefined', '-Wno-unused-parameter', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier',
                '-I', str(tree / '.cache/xnu-work/osfmk'),
                '-I', str(tree / 'build/storage-abi/include')]
    return command


def verify_object(data):
    magic, cpu, subtype, kind, count, commands_size = struct.unpack_from('<6I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise ValueError('expected an ARM64 Mach-O object')
    at = 32
    end = at + commands_size
    if end > len(data):
        raise ValueError('truncated Mach-O commands')
    deployment = None
    for _ in range(count):
        if at + 8 > end:
            raise ValueError('truncated load command')
        command, length = struct.unpack_from('<II', data, at)
        if length < 8 or at + length > end:
            raise ValueError('invalid load command size')
        if command == 0x32:
            if length < 24:
                raise ValueError('truncated build version')
            deployment = struct.unpack_from('<II', data, at + 8)
        at += length
    if at != end or deployment != (2, 0x110000):
        raise ValueError(f'expected iOS 17.0 deployment, got {deployment}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=REPO,
                        help='prepared XNU tree with storage ABI headers')
    parser.add_argument('--recipe', type=Path,
                        help='IOPlatformExpert.cpo.json from the matching XNU build')
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='new absolute directory; existing outputs are never overwritten')
    args = parser.parse_args()
    tree = args.tree.resolve()
    source = REPO / 'drivers' / COMPONENT / (COMPONENT + '.cpp')
    recipe = args.recipe or tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    if not args.output_dir.is_absolute() or args.output_dir.exists():
        parser.error('--output-dir must be a new absolute directory')
    out = args.output_dir.resolve()
    destination = out / (COMPONENT + '.o')
    command = compile_command(record, source, destination, tree)
    out.mkdir(parents=True, exist_ok=False)
    (out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'compile.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-4000:])
    verify_object(destination.read_bytes())
    archive = out / ('lib' + COMPONENT + '.a')
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(destination)], check=True)
    report = {'source': str(source), 'recipe': str(recipe), 'archive': str(archive),
              'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'target': 'arm64-apple-ios17.0', 'werror': True, 'hardware_tested': False}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
