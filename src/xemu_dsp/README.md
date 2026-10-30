# xemu's MCPX DSP (vendored for D-012)

- **Source:** xemu v0.8.139 (`hw/xbox/mcpx/apu/dsp/`: `dsp.c`, `dsp.h`, `dsp_c.c`, `dsp_dma.*`, `dsp_internal.h`, `debug.h`, `interp/`). The JIT is not vendored.
- **License:** GPL-2.0-or-later as the files' headers say (`dsp_dma.*`: LGPL-2.0-or-later). Our additions below are GPL-2.0-or-later too (SPDX line at the top of each).
- **Our additions:**
  - `shim/`: stand-ins for the few QEMU/GLib pieces it uses (`g_new0`, `g_free`, `ARRAY_SIZE`, `MIN`, `ssize_t`, `ldl_le_p`/`stl_le_p`, trace hooks, `g_config`);
  - `bw_dsp.[ch]`: an opaque wrapper, because the toolkit's APU headers also define a `DSPState`;
  - `CMakeLists.txt`.
- **Changes to xemu's files:** decoder tables are initialized once and the
  opcode cache is thread-local; DMA scratch storage belongs to each DSP and is
  released on teardown. Allocation failures are reported to the wrapper.
- **The glue** (xemu's `gp_ep.c` equivalent) is `src/toolkit_patches/apu/bw_apu_xdsp.c`.
  Its DSP-window merge snapshots aligned guest words and uses compare/exchange
  for writeback, so an intervening guest write is not overwritten.
- **Host requirements:** C11 and the platform thread library (`Threads::Threads`).
  The APU uses the interpreter on either a joinable worker or the frame thread;
  its real macOS output is the carried SDL2 backend, not a DSP stub. Device or
  frame-worker startup failure makes `mcpx_apu_init_standalone` return `NULL`;
  `mcpx_apu_shutdown` joins the workers before guest RAM can be released.
- **Synthetic tests:** configure `tests/apu_output` with CMake, build it, then
  run `ctest --output-on-failure` in that build directory. The lifecycle tests
  use synthetic guest RAM and `SDL_AUDIODRIVER=dummy` explicitly; production
  does not silently select the dummy driver when a device is unavailable.
