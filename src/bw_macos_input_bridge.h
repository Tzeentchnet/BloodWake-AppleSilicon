#ifndef BW_MACOS_INPUT_BRIDGE_H
#define BW_MACOS_INPUT_BRIDGE_H

/* Call only before guest threads start, or after they have stopped. */
void bw_macos_input_bridge_configure(int enabled);

#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
void bw_macos_input_bridge_trace_synthetic(int enabled);
#endif

#endif
