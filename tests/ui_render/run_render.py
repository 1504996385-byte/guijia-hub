"""Render actual production UI with the installed LVGL8 software renderer.

Only board APIs are stubbed. LVGL, fonts, UI and face model are compiled C.
The LVGL object factory is wrapped to count allocations, without changing its
implementation. The installed SDK lv_conf.h is used with its real RGB565 mode.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SDK = ROOT.parent / "TuyaOpen"
LVGL = SDK / "src/liblvgl/v8/lvgl"
BUILD = HERE / ".build"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--no-emotion", action="store_true", help="Test old UI before emotion API exists")
    parser.add_argument("--log", type=Path, default=HERE / "render-result.txt")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    compiler = runner.compiler_path(args.compiler)
    BUILD.mkdir(exist_ok=True)
    includes = [HERE / "stubs", ROOT / "firmware/core", ROOT / "firmware/src", LVGL,
                SDK / "src/liblvgl/v8/conf", SDK / "src/liblvgl/v8/port"]
    flags = ["-std=c99", "-include", "inttypes.h", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_LVGL_H_INCLUDE_SIMPLE",
             "-DLV_FONT_MONTSERRAT_14=1", "-DHH_RENDER_WITH_EMOTION=" + str(int(not args.no_emotion))]
    for path in includes:
        flags.extend(["-I", str(path)])
    sources = sorted((LVGL / "src").rglob("*.c"))
    sources.extend([ROOT / "firmware/src/homehub_ui.c", ROOT / "firmware/src/homehub_font_22.c",
                    SDK / "src/ai_components/ai_ui/font/font_puhui_16_2.c", HERE / "test_render.c"])
    face = ROOT / "firmware/core/homehub_face.c"
    if not args.no_emotion:
        if not face.exists():
            face = ROOT / "firmware/src/homehub_face.c"
        if face.exists():
            sources.append(face)
    # Header changes invalidate the cache too: this is a render of current C,
    # never an accidentally retained executable from an older UI implementation.
    header_hash = hashlib.sha256()
    for base in [LVGL / "src", SDK / "src/liblvgl/v8/conf", HERE / "stubs", ROOT / "firmware/core", ROOT / "firmware/src"]:
        for path in sorted(base.rglob("*.h")):
            header_hash.update(path.read_bytes())
    cache_key = hashlib.sha256((compiler + repr(flags) + header_hash.hexdigest()).encode()).hexdigest()

    def compile_one(source: Path) -> tuple[Path, str]:
        identity = hashlib.sha256(str(source).encode()).hexdigest()[:12]
        obj = BUILD / (source.stem + "-" + identity + ".o")
        stamp = obj.with_suffix(".sha256")
        digest = hashlib.sha256((cache_key + source.read_text(encoding="utf-8")).encode()).hexdigest()
        if obj.exists() and stamp.exists() and stamp.read_text() == digest:
            return obj, ""
        extra = []
        if source == LVGL / "src/core/lv_obj_class.c":
            extra = ["-Dlv_obj_class_create_obj=hh_actual_lv_obj_class_create_obj"]
        result = subprocess.run([compiler, *flags, *extra, "-c", str(source), "-o", str(obj)],
                                cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(str(source) + "\n" + result.stdout + result.stderr)
        stamp.write_text(digest)
        return obj, result.stdout + result.stderr

    print("Compiling actual LVGL8/UI:", len(sources), "C files", flush=True)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            compiled = list(pool.map(compile_one, sources))
    except RuntimeError as error:
        print(error)
        return 1
    warnings = "".join(item[1] for item in compiled)
    if warnings:
        print(warnings, end="")
    binary = BUILD / "ui-render-tests.exe"
    result = subprocess.run([compiler, *[str(item[0]) for item in compiled], "-o", str(binary)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        return result.returncode
    provenance = {"ui_sha256": hashlib.sha256((ROOT / "firmware/src/homehub_ui.c").read_bytes()).hexdigest(),
                  "sdk_lvgl": str(LVGL), "sdk_config": str(SDK / "src/liblvgl/v8/conf/lv_conf.h"),
                  "renderer": "actual LVGL8 software RGB565 display flush", "compiler": compiler,
                  "sources": [str(source) for source in sources]}
    (BUILD / "provenance.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    result = subprocess.run([str(binary), str(BUILD)], cwd=ROOT, capture_output=True, text=True, timeout=60)
    output = result.stdout + result.stderr
    exit_code = result.returncode
    if not args.no_emotion:
        for emotion in range(5):
            pose = subprocess.run([str(binary), str(BUILD), str(emotion)], cwd=ROOT,
                                  capture_output=True, text=True, timeout=15)
            output += pose.stdout + pose.stderr
            exit_code = exit_code or pose.returncode
        expression_files = [BUILD / ("pose-" + name + ".bmp") for name in
                            ["10-emotion-neutral", "11-happy", "12-angry", "13-sad", "14-joy"]]
        if len({path.read_bytes() for path in expression_files}) == 5:
            output += "PASS five reply expressions differ at identical animation phase in fresh processes\n"
        else:
            output += "FAIL expressions are identical at matched animation phase\n"
            exit_code = exit_code or 1
    print(output, end="", flush=True)
    args.log.write_text(output, encoding="utf-8")
    # BMP is the primary framebuffer artifact. PNG/contact sheet conversion is
    # optional and never substitutes a different renderer.
    try:
        from PIL import Image, ImageDraw
        files = sorted(BUILD.glob("pose-*.bmp"))
        sheet = Image.new("RGB", (466 * 4, 502 * ((len(files) + 3) // 4)), "#202020")
        draw = ImageDraw.Draw(sheet)
        for i, path in enumerate(files):
            image = Image.open(path).convert("RGB")
            image.save(path.with_suffix(".png"))
            x, y = (i % 4) * 466, (i // 4) * 502
            sheet.paste(image, (x, y + 28))
            draw.text((x + 10, y + 6), path.stem.removeprefix("pose-"), fill="white")
        sheet.save(BUILD / "contact-sheet.png")
    except ImportError:
        print("Pillow unavailable: actual framebuffer BMPs retained")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
