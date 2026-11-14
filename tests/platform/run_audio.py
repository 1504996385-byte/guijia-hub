"""Compile actual board audio/storage C against host TKL and PCM callbacks."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = HERE / ".build"
build.mkdir(exist_ok=True)
binary = build / "audio-tests.exe"
command = [runner.compiler_path(None), "-std=c99", "-Wall", "-Werror", "-I", str(HERE / "stubs"),
           "-I", str(ROOT / "firmware/core"),
           str(HERE / "test_audio.c"), "-o", str(binary)]
result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if result.returncode:
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)
with tempfile.TemporaryDirectory(prefix="homehub-audio-") as runtime:
    (Path(runtime) / "messages").mkdir()
    result = subprocess.run([str(binary), runtime], capture_output=True, text=True)
    output = result.stdout + result.stderr
    print(output, end="")
    (HERE / "audio-result.txt").write_text(output, encoding="utf-8")
    raise SystemExit(result.returncode)
