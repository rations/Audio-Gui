/*
 * bridge_peak.h
 * Shared single-writer / single-reader peak-level channel between a pulse bridge
 * (writer) and the Audio-Gui level meter (reader).
 *
 * The bridge already mixes every PA-compat stream into a stereo float buffer once
 * per period (pulse_alsa_bridge.c:play_loop, pulse_jack_bridge.c:jack_process).
 * After that mix it stores the block peak here; the GUI maps this page read-only
 * and animates a meter from it. Plain memory stores only — no locks, RT-safe.
 *
 * Liveness: the writer bumps `seq` every block. If the GUI sees `seq` stop
 * advancing (bridge stopped/crashed) it lets the meter decay to zero.
 *
 * The page is a POSIX shared-memory object, created 0600 (per-user) by the
 * bridge and mapped PROT_READ by the GUI. Both bridges write the SAME object, so
 * only one bridge runs at a time (they are mutually exclusive on the PA socket).
 */
#ifndef BRIDGE_PEAK_H
#define BRIDGE_PEAK_H

#include <stdint.h>

#define BRIDGE_PEAK_SHM "/pulse_bridge_peak"

struct BridgePeak
{
  volatile uint32_t seq; /* incremented once per published block (liveness) */
  volatile float peakL; /* block peak |L|, linear 0.0 .. ~1.0 */
  volatile float peakR; /* block peak |R| */
  volatile uint32_t rate; /* output sample rate (Hz), informational */
};

#endif /* BRIDGE_PEAK_H */
