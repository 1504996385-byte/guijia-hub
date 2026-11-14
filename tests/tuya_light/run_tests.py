"""Test actual light source, SDK mbedTLS crypto and cJSON with synthetic HTTP."""
import argparse
import hashlib
import hmac
import importlib.util
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--red', action='store_true')
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('core_runner', ROOT/'tests/core/run_tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = HERE/'.build'
build.mkdir(exist_ok=True)
# Independent standard-library oracle. All inputs are synthetic.
timestamp = '1793923200000'
on = '{"commands":[{"code":"switch_led","value":true},{"code":"work_mode","value":"colour"},{"code":"colour_data","value":{"h":35,"s":900,"v":800}}]}'
def signature(method, path, body, token=''):
    canonical = method+'\n'+hashlib.sha256(body.encode()).hexdigest()+'\n\n'+path
    return hmac.new(b'example-secret', ('example-client'+token+timestamp+canonical).encode(), hashlib.sha256).hexdigest().upper()
(build/'vectors.h').write_text(
    '#define TOKEN_SIGN "'+signature('GET','/v1.0/token?grant_type=1','')+'"\n'
    '#define ON_SIGN "'+signature('POST','/v1.0/devices/example-device/commands',on,'test-access-token')+'"\n', encoding='utf-8')
sdk = ROOT.parent/'TuyaOpen'
crypto = sdk/'src/libtls/mbedtls-3.1.0'
cjson = sdk/'src/libcjson/cJSON'
command = [runner.compiler_path(None), '-std=c99', '-Wall', '-Werror',
           '-DMBEDTLS_CONFIG_FILE="hh_mbedtls_config.h"',
           '-I', str(HERE/'stubs'), '-I', str(build), '-I', str(ROOT/'firmware/core'),
           '-I', str(ROOT/'firmware/src'), '-I', str(crypto/'include'), '-I', str(cjson),
           str(HERE/'test_light.c'), str(crypto/'library/md.c'),
           str(crypto/'library/sha256.c'), str(crypto/'library/platform_util.c'), str(crypto/'library/platform.c'),
           str(cjson/'cJSON.c'), '-o', str(build/'light-tests.exe')]
if args.red:
    command.insert(1, '-DHH_LIGHT_RED')
r = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if r.returncode == 0:
    r = subprocess.run([str(build/'light-tests.exe')], cwd=ROOT, capture_output=True, text=True)
output = r.stdout+r.stderr
print(output, end='')
(HERE/('red-result.txt' if args.red else 'green-result.txt')).write_text(output, encoding='utf-8')
if args.red:
    raise SystemExit(0 if r.returncode == 1 and 'FAIL ' in output and 'failure(s)' in output else 1)
raise SystemExit(r.returncode)
