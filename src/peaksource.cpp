// See peaksource.h.

#include "peaksource.h"

#include "bridge_peak.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
// Ticks without a new `seq` before the mapping is dropped. At the 33 ms meter tick that is about
// a tenth of a second -- long enough not to trip over a bridge that missed one block, short
// enough that a restarted bridge is picked up before the bar has finished falling.
constexpr int kStallLimit = 3;
} // namespace

PeakSource::~PeakSource()
{
    close();
}

bool PeakSource::attach()
{
    if (m_peak)
        return true;

    // Read-only: the GUI only ever consumes what the bridge publishes.
    m_fd = shm_open(BRIDGE_PEAK_SHM, O_RDONLY, 0);
    if (m_fd < 0)
        return false;

    void *p = mmap(nullptr, sizeof(BridgePeak), PROT_READ, MAP_SHARED, m_fd, 0);
    if (p == MAP_FAILED) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    m_peak = static_cast<const volatile BridgePeak *>(p);
    return true;
}

void PeakSource::close()
{
    if (m_peak) {
        munmap(const_cast<void *>(static_cast<const volatile void *>(m_peak)), sizeof(BridgePeak));
        m_peak = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_lastSeq = 0;
    m_stallTicks = 0;
}

bool PeakSource::poll(float &l, float &r)
{
    if (!m_peak && !attach())
        return false;

    const uint32_t seq = m_peak->seq;
    if (seq == m_lastSeq) {
        if (++m_stallTicks >= kStallLimit) {
            // The bridge stopped publishing: exited, or was replaced. Drop the mapping so a newly
            // started bridge's object is picked up -- holding this one would keep us attached to
            // an inode nothing writes to any more.
            close();
        }
        return false;
    }

    m_lastSeq = seq;
    m_stallTicks = 0;

    // Copied out of the volatile fields before being handed on; the caller clamps.
    l = m_peak->peakL;
    r = m_peak->peakR;
    return true;
}
