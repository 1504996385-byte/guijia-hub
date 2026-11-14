"""Check the actual event header's memory budget and complete queue payloads."""
import importlib.util
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('core_runner', ROOT / 'tests/core/run_tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = ROOT / 'tests/platform/.build/event_memory'
build.mkdir(parents=True, exist_ok=True)
binary = build / 'event-memory.exe'
cmd = [runner.compiler_path(None), '-std=c99', '-Wall', '-Werror',
       '-I', str(HERE / 'stubs'), '-I', str(ROOT / 'firmware/src'),
       '-I', str(ROOT / 'firmware/core'), str(HERE / 'test_event_memory.c'),
       '-o', str(binary)]
result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
if not result.returncode:
    result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
output = result.stdout + result.stderr
print(output, end='')
(HERE / (sys.argv[1] if len(sys.argv)>1 else 'green-result.txt')).write_text(output, encoding='utf-8')
raise SystemExit(result.returncode)
