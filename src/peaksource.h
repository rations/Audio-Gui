// Reading the block peaks the running bridge publishes.
//
// This is the Qt LevelMeter's shared-memory half, lifted out unchanged in substance. It was never
// Qt code -- shm_open/mmap and four volatile fields -- and it is separated from the drawing so
// that gfx/levelmeter.cpp links cairo and nothing else, which is what lets tools/uirender draw a
// meter at a chosen level with no bridge running and no X server.
//
// The page is written by whichever bridge is up (pulse_alsa_bridge.c:peak_publish, and the same
// function in pulse_jack_bridge.c), single-writer, lock-free, plain volatile stores. Only one
// bridge runs at a time -- they bind the same PA socket -- so there is exactly one writer.
//
// ATTACHING IS LAZY AND REPEATED, because the bridge starts and stops while the GUI runs: the
// user switches routing mode, or the bridge is restarted to change device. If `seq` stops
// advancing the mapping is torn down, so a NEW bridge's shm object (a fresh inode under the same
// name) is picked up rather than the old, unlinked one being held forever.

#pragma once

#include <cstdint>

struct BridgePeak;

class PeakSource
{
public:
    PeakSource() = default;
    ~PeakSource();

    PeakSource(const PeakSource &) = delete;
    PeakSource &operator=(const PeakSource &) = delete;

    // Poll for a new block peak. Returns true and fills l/r when the bridge has published
    // something since the last call; false when it has not, which is both "silence" and "no
    // bridge" -- the meter treats them the same, because to a listener they are the same.
    bool poll(float &l, float &r);

    void close();

private:
    bool attach();

    int m_fd = -1;
    const volatile BridgePeak *m_peak = nullptr;
    uint32_t m_lastSeq = 0;
    int m_stallTicks = 0;
};
