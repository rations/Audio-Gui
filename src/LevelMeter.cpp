// LevelMeter.cpp
#include "LevelMeter.h"

#include <QLinearGradient>
#include <QPainter>
#include <QTimer>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "bridge_peak.h"

namespace
{
constexpr int kFps = 30;
constexpr float kDecay = 0.18f; // per-tick fall when input is below current
constexpr int kStallLimit = 3; // ticks without new seq before we go silent
} // namespace

LevelMeter::LevelMeter(QWidget* parent)
: QWidget(parent)
{
  setMinimumHeight(28);
  m_timer = new QTimer(this);
  m_timer->setInterval(1000 / kFps);
  connect(m_timer, &QTimer::timeout, this, &LevelMeter::tick);
  m_timer->start();
}

LevelMeter::~LevelMeter()
{
  closeShm();
}

QSize LevelMeter::sizeHint() const
{
  return QSize(240, 36);
}

void LevelMeter::setThemeColors(const QColor& background, const QColor& track)
{
  m_bgColor = background;
  m_trackColor = track;
  update();
}

bool LevelMeter::openShm()
{
  if (m_peak)
    return true;
  // Read-only: the GUI only ever consumes what the bridge publishes.
  m_shmFd = shm_open(BRIDGE_PEAK_SHM, O_RDONLY, 0);
  if (m_shmFd < 0)
    return false;
  void* p = mmap(nullptr, sizeof(BridgePeak), PROT_READ, MAP_SHARED, m_shmFd, 0);
  if (p == MAP_FAILED)
  {
    ::close(m_shmFd);
    m_shmFd = -1;
    return false;
  }
  m_peak = static_cast<BridgePeak*>(p);
  return true;
}

void LevelMeter::closeShm()
{
  if (m_peak)
  {
    munmap(m_peak, sizeof(BridgePeak));
    m_peak = nullptr;
  }
  if (m_shmFd >= 0)
  {
    ::close(m_shmFd);
    m_shmFd = -1;
  }
}

void LevelMeter::tick()
{
  float targetL = 0.0f, targetR = 0.0f;

  // (Re)attach to the shm page; the bridge may start/stop while we run.
  if (!m_peak)
    openShm();

  if (m_peak)
  {
    unsigned seq = m_peak->seq;
    if (seq != m_lastSeq)
    {
      m_lastSeq = seq;
      m_stallTicks = 0;
      // Copy out of the volatile shm fields before clamping (std::clamp can't
      // deduce a volatile template argument).
      const float pl = m_peak->peakL;
      const float pr = m_peak->peakR;
      targetL = std::clamp(pl, 0.0f, 1.0f);
      targetR = std::clamp(pr, 0.0f, 1.0f);
    }
    else if (++m_stallTicks >= kStallLimit)
    {
      // Bridge stopped publishing (exited or paused) — drop our handle so a
      // newly started bridge gets picked up, and fall to silence.
      closeShm();
      m_lastSeq = 0;
      m_stallTicks = 0;
    }
  }

  // Fast attack (jump up immediately), slow decay (ease down).
  m_dispL = targetL > m_dispL ? targetL : m_dispL + (targetL - m_dispL) * kDecay;
  m_dispR = targetR > m_dispR ? targetR : m_dispR + (targetR - m_dispR) * kDecay;
  if (m_dispL < 0.0005f)
    m_dispL = 0.0f;
  if (m_dispR < 0.0005f)
    m_dispR = 0.0f;

  update();
}

void LevelMeter::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const int gap = 4;
  const int barH = (height() - gap) / 2;
  const int w = width();
  const qreal radius = 4.0;

  // green → yellow → red, so loud signals read as hot.
  QLinearGradient grad(0, 0, w, 0);
  grad.setColorAt(0.0, QColor(46, 204, 113));
  grad.setColorAt(0.7, QColor(241, 196, 15));
  grad.setColorAt(1.0, QColor(231, 76, 60));

  p.fillRect(rect(), m_bgColor);

  auto drawBar = [&](int y, float level) {
    QRectF track(0, y, w, barH);
    p.setPen(Qt::NoPen);
    p.setBrush(m_trackColor);
    p.drawRoundedRect(track, radius, radius);
    int fill = static_cast<int>(std::lround(level * w));
    if (fill > 0)
    {
      p.setBrush(grad);
      p.drawRoundedRect(QRectF(0, y, fill, barH), radius, radius);
    }
  };

  drawBar(0, m_dispL);
  drawBar(barH + gap, m_dispR);
}
