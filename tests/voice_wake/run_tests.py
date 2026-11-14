"""Compile the actual T5 wake adapter and actual core against pinned SDK APIs."""
import importlib.util
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('core_runner', ROOT/'tests/core/run_tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = ROOT/'tests/platform/.build/voice_wake'
build.mkdir(parents=True,exist_ok=True)
adapter = ROOT.parent/'TuyaOpen/platform/T5AI/tuyaos/tuyaos_adapter/include'
command = [runner.compiler_path(None), '-std=c99', '-Wall', '-Wextra', '-Werror',
           '-I', str(HERE/'stubs'), '-I', str(ROOT/'firmware/src'),
           '-I', str(ROOT/'firmware/core'), '-I', str(adapter/'kws'),
           '-I', str(adapter/'vad'), str(HERE/'test_voice_wake.c'),
           str(ROOT/'firmware/core/homehub_core.c'), '-o', str(build/'voice-tests.exe')]
result = subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
if not result.returncode:
    result = subprocess.run([str(build/'voice-tests.exe')],cwd=ROOT,capture_output=True,text=True)
output = result.stdout+result.stderr
print(output,end='')
(HERE/(sys.argv[1] if len(sys.argv)>1 else 'green-result.txt')).write_text(output,encoding='utf-8')
raise SystemExit(result.returncode)
