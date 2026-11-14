"""Package the actual cross-compiled image and non-secret TF-card assets."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from datetime import datetime, timezone
import zipfile
from release_tools import BOARD, PROTECTED_START, validate_release

ROOT = Path(__file__).resolve().parents[1]


def revision(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT / 'firmware/dist/firmware_1.0.0/firmware_QIO_1.0.0.bin')
    parser.add_argument('--partitions', type=Path, default=ROOT / 'firmware/dist/firmware_1.0.0/debug/auto_partitions.csv')
    parser.add_argument('--sdk', type=Path, default=Path(r'D:\Tuya\TuyaOpen'))
    args = parser.parse_args()
    image = args.image.resolve()
    if not image.is_file() or '_QIO_' not in image.name or 'vendor' in image.parts:
        raise SystemExit('Expected an actual project-built QIO image, not vendor or OTA firmware')
    rows = csv.reader(args.partitions.read_text(encoding='utf-8-sig').splitlines())
    partitions = {r[0].strip(): r[1].strip() for r in rows
                  if len(r) > 1 and r[0].strip() and not r[0].lstrip().startswith('#')}
    if int(partitions.get('tuya_data', '0'), 16) != PROTECTED_START:
        raise SystemExit('Actual partition layout differs from the flashing protection boundary')
    output = ROOT / 'dist'
    output.mkdir(exist_ok=True)
    target = output / 'homehub-1.0.0-QIO.bin'
    shutil.copyfile(image, target)
    shutil.copyfile(args.partitions, output / 'auto_partitions.csv')
    sources = {}
    for p in sorted((ROOT / 'firmware').rglob('*')):
        if p.is_file() and '.build' not in p.parts and 'dist' not in p.parts and (
                p.suffix in ('.c', '.h', '.config') or p.name == 'CMakeLists.txt'):
            sources[p.relative_to(ROOT).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = {
        'board': BOARD, 'version': '1.0.0', 'created_at': datetime.now(timezone.utc).isoformat(),
        'firmware': {'file': target.name, 'bytes': target.stat().st_size,
                     'sha256': hashlib.sha256(target.read_bytes()).hexdigest(), 'start_address': 0},
        'protected_region_start': PROTECTED_START, 'partitions': partitions,
        'sdk_commit': revision(args.sdk), 'platform_commit': revision(args.sdk / 'platform/T5AI'),
        'source_sha256': sources,
        'validation': {'cross_compiled': True, 'hardware_connected': False,
                       'hardware_flashed': False, 'physical_inputs_audio_sd_tested': False,
                       'cloud_services_live_tested': False},
        'recording': {'control': 'physical button hold/release', 'max_seconds': 30,
                      'sample_rate': 16000, 'bits': 16, 'channels': 1},
        'pins': {'reed_0': 14, 'reed_1': 15, 'record_0': 16, 'record_1': 17, 'optional_pir': 47},
        'test_voice_source': 'Windows synthetic Mandarin; not human/dialect/AI live speech'
    }
    path = output / 'release.json'
    path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    validate_release(path)
    with zipfile.ZipFile(output / 'homehub-sdcard.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted((ROOT / 'sdcard/homehub').rglob('*')):
            if p.is_file() and p.name != 'config.json':
                z.write(p, 'homehub/' + p.relative_to(ROOT / 'sdcard/homehub').as_posix())
        z.writestr('homehub/config.json', (ROOT / 'sdcard/homehub/config.example.json').read_bytes())
    print(json.dumps({'manifest': str(path), 'image': str(target), 'bytes': target.stat().st_size,
                      'sha256': manifest['firmware']['sha256'], 'sdcard_zip': str(output / 'homehub-sdcard.zip')}, indent=2))


if __name__ == '__main__':
    main()
