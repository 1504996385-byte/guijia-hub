"""Actual SDK coreHTTP+parser; synthetic TLS transport, no sockets or keys."""
import importlib.util
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
SDK=ROOT.parent/'TuyaOpen'
spec=importlib.util.spec_from_file_location('core_runner',ROOT/'tests/core/run_tests.py')
runner=importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build=ROOT/'tests/platform/.build/cloud_http'
build.mkdir(parents=True,exist_ok=True)
http=SDK/'src/libhttp/coreHTTP'
compiler=runner.compiler_path(None)
flags=['-D__WINE__',
         '-I',str(HERE/'stubs'),'-I',str(ROOT/'firmware/src'),
         '-I',str(SDK/'src/tuya_cloud_service/transport'),'-I',str(SDK/'src/tuya_cloud_service/tls'),
         '-I',str(SDK/'src/libmqtt/include'),'-I',str(http/'source/include'),
         '-I',str(http/'source/dependency/3rdparty/http_parser')]
objects=[]
# Match the production SDK libhttp -w setting; our own adapter/tests retain
# -Wall -Werror. The pinned SDK has existing const-discard warnings.
for source in [http/'source/core_http_client.c',http/'source/dependency/3rdparty/http_parser/http_parser.c']:
    obj=build/(source.stem+'.o')
    built=subprocess.run([compiler,'-std=c99','-w',*flags,'-c',str(source),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
    if built.returncode:
        print(built.stdout+built.stderr,end='')
        raise SystemExit(built.returncode)
    objects.append(str(obj))
command=[compiler,'-std=c99','-Wall','-Werror',*flags,
         str(HERE/'test_cloud_http.c'),str(ROOT/'firmware/src/homehub_cloud_http.c'),*objects,
         '-o',str(build/'cloud-http-tests.exe')]
result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
if not result.returncode:
    result=subprocess.run([str(build/'cloud-http-tests.exe')],cwd=ROOT,capture_output=True,text=True)
output=result.stdout+result.stderr
print(output,end='')
(HERE/(sys.argv[1] if len(sys.argv)>1 else 'green-result.txt')).write_text(output,encoding='utf-8')
raise SystemExit(result.returncode)
