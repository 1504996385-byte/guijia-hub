"""Actual installer/TF-only seeder with fault-injected synthetic storage."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import zlib

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
spec=importlib.util.spec_from_file_location('core_runner',ROOT/'tests/core/run_tests.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
build=HERE/'.build';build.mkdir(exist_ok=True)
storage=(ROOT/'firmware/src/homehub_storage.c').read_text(encoding='utf-8')
helpers=storage[storage.index('uint32_t hh_checksum('):storage.index('bool hh_valid_id(')]
helpers+=storage[storage.index('static uint32_t u32('):storage.index('static void copy_json(')]
(build/'actual_wav_helpers.h').write_text(helpers,encoding='utf-8')
wav=struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',40,b'WAVE',b'fmt ',16,1,1,16000,32000,2,16,b'data',4)+b'\x12\x34\x56\x78'
checksum=2166136261
for b in wav:checksum=((checksum^b)*16777619)&0xffffffff
(build/'fixture.h').write_text('const uint8_t hh_reminder_wav[]={'+','.join(map(str,wav))+'};\n'
    +'uint32_t hh_reminder_wav_size=sizeof(hh_reminder_wav);\n'
    +f'uint32_t hh_reminder_wav_checksum={checksum}u;\n'
    +f'#define FIXTURE_CHECKSUM {checksum}u\n'
    +f'#define FIXTURE_OLD_BACKUP HH_ROOT "/voices/take_key.{zlib.crc32(wav[:44]+bytes([wav[44]^0x42])+wav[45:]):08x}.48.bak.wav"\n',encoding='utf-8')
departure=wav[:44]+b'\x21\x43\x65\x87'
departure_checksum=2166136261
for b in departure:departure_checksum=((departure_checksum^b)*16777619)&0xffffffff
with (build/'fixture.h').open('a',encoding='utf-8') as fixture:
    fixture.write('#define HH_REMINDER_ASSET_VERSION 3u\n#define HH_DEPARTURE_ASSET_VERSION 3u\n'
        +'const uint8_t hh_departure_wav[]={'+','.join(map(str,departure))+'};\n'
        +'uint32_t hh_departure_wav_size=sizeof(hh_departure_wav);\n'
        +f'uint32_t hh_departure_wav_checksum={departure_checksum}u;\n'
        +f'#define FIXTURE_DEPARTURE_CHECKSUM {departure_checksum}u\n'
        +f'#define FIXTURE_OLD_DEPARTURE_BACKUP HH_ROOT "/voices/leave.{zlib.crc32(wav[:44]+bytes([wav[44]^0x42])+wav[45:]):08x}.48.bak.wav"\n')
arrival=wav[:44]+b'\x31\x53\x75\x97'
arrival_checksum=2166136261
for b in arrival:arrival_checksum=((arrival_checksum^b)*16777619)&0xffffffff
with (build/'fixture.h').open('a',encoding='utf-8') as fixture:
    fixture.write('#define HH_ARRIVAL_ASSET_VERSION 3u\n'
        +'const uint8_t hh_arrival_wav[]={'+','.join(map(str,arrival))+'};\n'
        +'uint32_t hh_arrival_wav_size=sizeof(hh_arrival_wav);\n'
        +f'uint32_t hh_arrival_wav_checksum={arrival_checksum}u;\n'
        +f'#define FIXTURE_ARRIVAL_CHECKSUM {arrival_checksum}u\n'
        +f'#define FIXTURE_OLD_ARRIVAL_BACKUP HH_ROOT "/voices/arrive.{zlib.crc32(wav[:44]+bytes([wav[44]^0x42])+wav[45:]):08x}.48.bak.wav"\n')
cmd=[runner.compiler_path(None),'-std=c99','-Wall','-Wextra','-Werror','-DHOMEHUB_EMBED_TEST_VOICES=0',
     '-I',str(HERE/'stubs'),'-I',str(build),'-I',str(ROOT/'firmware/src'),'-I',str(ROOT/'firmware/core'),
     str(HERE/'test_reminder_install.c'),'-o',str(build/'reminder-tests.exe')]
r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True)
if not r.returncode:r=subprocess.run([str(build/'reminder-tests.exe')],cwd=ROOT,capture_output=True,text=True)
output=r.stdout+r.stderr;print(output,end='')
logs=[arg for arg in sys.argv[1:] if not arg.startswith('--')]
(HERE/(logs[0] if logs else 'green-result.txt')).write_text(output,encoding='utf-8')
raise SystemExit(r.returncode)
