# Actual LVGL8 host rendering

Run from the checkout:

```powershell
python tests/ui_render/run_render.py
```

This compiles the installed `../TuyaOpen/src/liblvgl/v8/lvgl/src` C sources,
the SDK's `lv_conf.h`, production `homehub_ui.c` and `homehub_face.c`, and the
actual production/SDK fonts. The host display is 466 x 466 RGB565 with a flush
callback writing a framebuffer. It uses neither SDL nor a substitute renderer.
Only board display, clock, logging, memory and queue APIs are stubbed. The
project's existing pinned TinyCC runner provides the compiler; `--compiler`
can select another host compiler. The host explicitly enables the SDK's
default Montserrat14 font and includes standard `inttypes.h` for TinyCC.

Outputs are under `.build/` (ignored): raw framebuffer BMPs, optional lossless
PNG conversions/contact sheet when Pillow is available, compiled objects and
`provenance.json`. `render-result.txt` records the behavior assertions.
The source/header digest cache recompiles any affected C source. Each run
always relinks and executes the current production code.

Coverage includes approved neutral eye dimensions/positions and RGB565 colors,
no visible main-screen text/buttons, no canvas/image, five expressions and
operational poses, pixel continuity, exact incremental/full redraw equality,
long press settings/return, recipient cycling/rejected queue post, zero queue
wait, idle dim/wake, and zero additional object factory allocations during
animation and settings. Expression screenshots also run in five fresh
processes at the same absolute animation phase, so ordinary animation cannot
masquerade as expression differences. The real LVGL object factory is wrapped
only to count calls; its original implementation is compiled and called.

The initial old UI failed nine behavior assertions (`baseline-red-result.txt`).
Visual inspection subsequently identified dark gaps through rounded primitive
centers; `round-center-red-result.txt` records the corresponding real pixel
regressions. A full redraw reproduced the same gaps, ruling out stale dirty
regions. The SDK RGB565 partial-opacity fast path rounded opacity 252 to 256
and overflowed its 8-bit opacity to zero, leaving unmasked center fills dark.
The production renderer now clamps that near-opaque boundary. The same center
pixel regressions pass without modifying SDK code or adding objects. The final
run records 64 main-scenario assertions plus five isolated refresh checks, one
isolated tear-center check and the matched-phase expression comparison (71
passes total), with zero additional LVGL objects after initialization.

This is host software rendering and simulated UI events. It does not verify
touch-controller wiring, panel appearance/brightness, board performance, or
actual cloud tag delivery. The square framebuffer includes pixels outside the
physical circular panel: its corners expose the SDK bottom layer and are not
part of the real round display.
