"""Validate a HomeHub release, prepare a TF card, and flash with a local backup.

No device write occurs unless the explicit `flash --port COMx` command is used.
Flash backups can contain credentials and stay in the ignored backups directory.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'WAVESHARE_T5AI_TOUCH_AMOLED_1_75'
PROTECTED_START = 0x7CD000


def _validated_payload(manifest):
    manifest = Path(manifest).resolve()
    info = json.loads(manifest.read_text(encoding='utf-8-sig'))
    if info.get('board') != BOARD:
        raise ValueError('Wrong board in release manifest')
    fw = info.get('firmware', {})
    if fw.get('start_address') != 0:
        raise ValueError('Only full firmware at start address 0 is accepted')
    size = fw.get('bytes', 0)
    if not isinstance(size, int) or size <= 0:
        raise ValueError('Empty firmware image')
    if ((size + 4095) // 4096) * 4096 > PROTECTED_START:
        raise ValueError('Firmware would overlap protected authorization/RF region')
    image = (manifest.parent / fw.get('file', '')).resolve()
    if not image.is_relative_to(manifest.parent) or not image.is_file():
        raise ValueError('Invalid firmware path')
    payload = image.read_bytes()
    if len(payload) != size:
        raise ValueError('Firmware byte count mismatch')
    actual = hashlib.sha256(payload).hexdigest()
    if actual.lower() != str(fw.get('sha256', '')).lower():
        raise ValueError('Firmware SHA256 mismatch')
    return image, payload


def validate_release(manifest):
    return _validated_payload(manifest)[0]


def prepare_card(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    target = destination / 'homehub'
    target.mkdir(parents=True, exist_ok=True)
    copied, preserved = [], []
    for item in source.rglob('*'):
        if not item.is_file():
            continue
        if item.is_symlink() or not item.resolve().is_relative_to(source):
            raise ValueError('Source card package contains a symlink')
        relative = item.relative_to(source)
        out = target / relative
        if not out.resolve().is_relative_to(target.resolve()):
            raise ValueError('Unsafe card destination path')
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists():
            preserved.append(str(relative))
            continue
        shutil.copyfile(item, out)
        copied.append(str(relative))
    config = target / 'config.json'
    example = target / 'config.example.json'
    if not config.exists() and example.is_file():
        shutil.copyfile(example, config)
    return {'copied': len(copied), 'preserved': len(preserved), 'target': str(target)}


def run_tool(tool, arguments, capture=False):
    result = subprocess.run([str(tool), *arguments], text=True, encoding='utf-8',
                            errors='replace', capture_output=capture, check=False)
    if result.returncode:
        raise RuntimeError('tyutool failed; do not retry a write without checking device state')
    return result.stdout if capture else ''


def select_flash_port(ports, port):
    matches = [p for p in ports if port.lower() in {
        str(p.get(k, '')).lower() for k in ('path', 'port', 'portName', 'port_name', 'name')}]
    if len(matches) != 1:
        raise ValueError('Requested COM port is not uniquely present; connect the board first')
    if 'log' in str(matches[0].get('portRole', '')).lower():
        raise ValueError('Selected port is the log port, not the flash/auth port')
    selected = matches[0]
    # tyutool 3.2.10 omits portRole on the real CH342 dual-UART board.
    # Confirm all independent USB/interface/name fields, never COM numbering.
    known_ch342_a = (selected.get('portRole') is None and
                     selected.get('usbVid') == 0x1A86 and selected.get('usbPid') == 0x55D2 and
                     selected.get('usbInterface') == 0 and
                     'SERIAL-A CH342' in str(selected.get('name', '')).upper())
    if selected.get('portRole') != 'flash_auth' and not known_ch342_a:
        raise ValueError('Unknown port role; verify the A flash/auth interface before writing')
    return matches[0]


def flash(manifest, port, tool):
    image, payload = _validated_payload(manifest)
    if not re.fullmatch(r'COM[1-9][0-9]*', port, re.IGNORECASE):
        raise ValueError('Use an actual Windows COM port, for example COM7')
    ports = json.loads(run_tool(tool, ['list-ports', '--json'], capture=True))
    select_flash_port(ports, port)
    backups = ROOT / 'backups'
    backups.mkdir(exist_ok=True)
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    staged = backups / ('staged-image-' + stamp + '-' + uuid.uuid4().hex[:8] + '.bin')
    with staged.open('xb') as output:
        output.write(payload)
    expected_hash = hashlib.sha256(payload).hexdigest()
    backup = backups / ('before-flash-' + stamp + '-' + uuid.uuid4().hex[:8] + '.bin')
    run_tool(tool, ['read', '-d', 't5ai', '-p', port, '-b', '921600',
                    '-s', '0x0', '-l', '0x800000', '-f', str(backup)])
    if not backup.is_file() or backup.stat().st_size != 0x800000:
        raise RuntimeError('Full 8 MiB backup was not obtained; flash was NOT started')
    backup.with_suffix('.sha256').write_text(
        hashlib.sha256(backup.read_bytes()).hexdigest() + '\n', encoding='ascii')
    if hashlib.sha256(staged.read_bytes()).hexdigest() != expected_hash:
        raise RuntimeError('Staged image changed during backup; flash was NOT started')
    run_tool(tool, ['write', '-d', 't5ai', '-p', port, '-b', '921600',
                    '-s', '0x0', '-f', str(staged)])
    print('Write and tool verification completed. Screen/audio/physical inputs still need inspection.')
    print('Original flash backup saved locally (may contain credentials): ' + str(backup))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    check = sub.add_parser('check', help='Validate release without accessing hardware')
    check.add_argument('--manifest', type=Path, default=ROOT / 'dist' / 'release.json')
    card = sub.add_parser('card', help='Copy TF package, preserving existing config and recordings')
    card.add_argument('--destination', required=True, type=Path)
    write = sub.add_parser('flash', help='Read full backup, then flash verified HomeHub image')
    write.add_argument('--port', required=True)
    write.add_argument('--manifest', type=Path, default=ROOT / 'dist' / 'release.json')
    write.add_argument('--tool', type=Path, default=ROOT / 'tools' / 'tyutool-cli-3.2.10' / 'tyutool_cli.exe')
    args = parser.parse_args()
    if args.command == 'check':
        print('Release validated: ' + str(validate_release(args.manifest)))
        print('No hardware was accessed or flashed.')
    elif args.command == 'card':
        print(json.dumps(prepare_card(ROOT / 'sdcard' / 'homehub', args.destination), ensure_ascii=False))
    else:
        flash(args.manifest, args.port, args.tool)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, json.JSONDecodeError) as exc:
        print('STOP: ' + str(exc), file=sys.stderr)
        sys.exit(1)
