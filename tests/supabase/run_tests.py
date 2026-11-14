"""Test actual Supabase C/cJSON against synthetic HTTP and filesystem failures."""
import importlib.util
from pathlib import Path
import subprocess
import sys
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
spec=importlib.util.spec_from_file_location('core_runner',ROOT/'tests/core/run_tests.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
build=HERE/'.build';build.mkdir(exist_ok=True)
cjson=ROOT.parent/'TuyaOpen/src/libcjson/cJSON'
cmd=[runner.compiler_path(None),'-std=c99','-Wall','-Wextra','-Werror','-I',str(HERE/'stubs'),'-I',str(ROOT/'firmware/src'),'-I',str(ROOT/'firmware/core'),'-I',str(cjson),str(HERE/'test_supabase.c'),str(cjson/'cJSON.c'),str(ROOT/'firmware/core/homehub_core.c'),'-o',str(build/'supabase-tests.exe')]
r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True)
if not r.returncode:r=subprocess.run([str(build/'supabase-tests.exe')],cwd=ROOT,capture_output=True,text=True)
output=r.stdout+r.stderr;print(output,end='')
(HERE/(sys.argv[1] if len(sys.argv)>1 else 'green-result.txt')).write_text(output,encoding='utf-8')
raise SystemExit(r.returncode)
