"""Compile actual TF-only voice validation without linking embedded assets."""
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
binary = build / "seed-card-tests.exe"
command = [runner.compiler_path(None), "-std=c99", "-Wall", "-Werror",
           "-DHOMEHUB_EMBED_TEST_VOICES=0", "-I", str(HERE / "stubs"),
           "-I", str(ROOT / "firmware/src"), str(HERE / "test_seed_card.c"),
           "-o", str(binary)]
result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if result.returncode:
    print(result.stdout + result.stderr, end="")
    raise SystemExit(result.returncode)
with tempfile.TemporaryDirectory(prefix="homehub-seed-card-") as runtime:
    (Path(runtime) / "voices").mkdir()
    result = subprocess.run([str(binary), runtime], capture_output=True, text=True)
    print(result.stdout + result.stderr, end="")
    raise SystemExit(result.returncode)
