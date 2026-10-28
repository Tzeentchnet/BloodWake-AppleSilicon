# xemu's MCPX DSP (vendored for D-012)

- **Source:** xemu v0.8.139 (`hw/xbox/mcpx/apu/dsp/`: `dsp.c`, `dsp.h`, `dsp_c.c`, `dsp_dma.*`, `dsp_internal.h`, `debug.h`, `interp/`). The JIT is not vendored.
- **License:** GPL-2.0-or-later as the files' headers say (`dsp_dma.*`: LGPL-2.0-or-later). Our additions below are GPL-2.0-or-later too (SPDX line at the top of each).
- **Our additions:**
  - `shim/`: stand-ins for the few QEMU/GLib pieces it uses (`g_new0`, `g_free`, `ARRAY_SIZE`, `MIN`, `ssize_t`, `ldl_le_p`/`stl_le_p`, trace hooks, `g_config`);
  - `bw_dsp.[ch]`: an opaque wrapper, because the toolkit's APU headers also define a `DSPState`;
  - `CMakeLists.txt`.
- **Changes to xemu's files:** none.
- **The glue** (xemu's `gp_ep.c` equivalent) is `src/toolkit_patches/apu/bw_apu_xdsp.c`.
