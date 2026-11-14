"""Execute actual first-boot seed C, with pinned T5 FatFS/error fault injection."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import wave

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--prove-existence-regression", action="store_true",
                    help="Require failure when a temporary copy regresses to 0/FALSE-only existence handling")
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = HERE / ".build"
build.mkdir(exist_ok=True)
binary = build / ("seed-existence-regression-tests.exe" if args.prove_existence_regression else "seed-tests.exe")
command = [runner.compiler_path(None), "-std=c99", "-Wall", "-Werror", "-I", str(HERE / "stubs"),
           "-I", str(ROOT / "firmware/src"), str(HERE / "test_seed.c"),
           str(ROOT / "firmware/src/homehub_seed_assets.c"), "-o", str(binary)]
if args.prove_existence_regression:
    actual = (ROOT / "firmware/src/homehub_seed.c").read_text(encoding="utf-8")
    before = "!checked_exists(path, &exists)"
    if before not in actual:
        raise SystemExit("Expected source pattern changed; inspect before applying targeted regression")
    mutated = actual.replace(before, "tkl_fs_is_exist(path, &exists) != OPRT_OK")
    temporary_source = build / "seed-existence-regression.c"
    temporary_source.write_text(mutated, encoding="utf-8")
    command.insert(1, '-DHH_SEED_SOURCE="' + temporary_source.as_posix() + '"')
built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if built.returncode:
    print(built.stdout + built.stderr)
    raise SystemExit(built.returncode)
with tempfile.TemporaryDirectory(prefix="homehub-seed-") as runtime:
    voices = Path(runtime) / "voices"
    voices.mkdir()
    result = subprocess.run([str(binary), runtime], capture_output=True, text=True)
    output = result.stdout + result.stderr
    if args.prove_existence_regression:
        print(output, end="")
        if result.returncode and "FAIL absent_files_are_seeded_with_exact_asset_bytes" in output:
            print("PASS original existence regression caught with real T5 FatFS return codes; production source unchanged")
            raise SystemExit(0)
        raise SystemExit("Expected existence regression was not caught")
    if not result.returncode:
        manifest = json.loads((ROOT / "firmware/src/homehub_seed_assets.json").read_text(encoding="utf-8"))
        assert manifest["source"] == "existing normalized ordinary Mandarin test WAVs"
        assert {item["name"] for item in manifest["assets"]} == {"arrive.wav", "leave.wav", "take_key.wav"}
        for asset in manifest["assets"]:
            seeded = (voices / asset["name"]).read_bytes()
            original = (ROOT / "sdcard/homehub/voices" / asset["name"]).read_bytes()
            digest = hashlib.sha256(seeded).hexdigest()
            assert seeded == original, "Embedded asset changed from original source WAV"
            assert len(seeded) == asset["bytes"] and digest == asset["sha256"]
            with wave.open(str(voices / asset["name"]), "rb") as wav:
                assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000)
                assert 0 < wav.getnframes() / wav.getframerate() <= 30
            output += f"PASS source fidelity {asset['name']} bytes={len(seeded)} sha256={digest}\n"
        output += "Boundary: host fault injection only; no serial, hardware TF write, or acoustic test.\n"
    print(output, end="")
    (HERE / "seed-result.txt").write_text(output, encoding="utf-8")
    raise SystemExit(result.returncode)
