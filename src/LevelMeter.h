// LevelMeter.h
// Horizontal stereo (L/R) peak meter. Reads the peak the running pulse bridge
// publishes in the shared-memory page (bridge_peak.h) at ~30 fps and paints two
// bars with fast-attack / slow-decay ballistics. When no bridge is running (shm
// missing or its sequence counter stalls) the bars decay to silence.
#pragma once

#include <QColor>
#include <QWidget>

class QTimer;
struct BridgePeak;

class LevelMeter : public QWidget
{
  Q_OBJECT

public:
  explicit LevelMeter(QWidget* parent = nullptr);
  ~LevelMeter() override;

  QSize sizeHint() const override;

  // Background + empty-track colors, set by the theme so the meter matches
  // dark/light mode. The green→yellow→red signal gradient is fixed.
  void setThemeColors(const QColor& background, const QColor& track);

protected:
  void paintEvent(QPaintEvent* event) override;

private slots:
  void tick();

private:
  bool openShm();
  void closeShm();

  int m_shmFd = -1;
  BridgePeak* m_peak = nullptr;
  unsigned m_lastSeq = 0;
  int m_stallTicks = 0; // consecutive ticks with no new data → treat as silent

  float m_dispL = 0.0f; // smoothed display levels (0..1)
  float m_dispR = 0.0f;
  QTimer* m_timer = nullptr;

  QColor m_bgColor{20, 22, 26}; // outer background (theme-set)
  QColor m_trackColor{34, 38, 44}; // empty bar track (theme-set)
};
