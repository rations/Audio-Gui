// MainWindow.cpp
#include "MainWindow.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "LevelMeter.h"
#include "MixerStripWidget.h"
#include "Theme.h"

namespace
{

// Install (idempotently) an XDG autostart entry that re-applies the saved routing
// at login via `audio-gui --restore`. This is what makes the default work without
// ever opening the GUI and restores the previous choice after a reboot — using a
// plain .desktop file, no systemd.
void ensureAutostartEntry()
{
  const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                        .absoluteFilePath(QStringLiteral("autostart"));
  QDir().mkpath(dir);
  const QString path = QDir(dir).absoluteFilePath(QStringLiteral("audio-gui-restore.desktop"));

  const QString exec = QCoreApplication::applicationFilePath();
  const QString contents = QStringLiteral(
                             "[Desktop Entry]\n"
                             "Type=Application\n"
                             "Name=Audio routing (restore)\n"
                             "Comment=Restore the saved PulseAudio-compat audio routing\n"
                             "Exec=\"%1\" --restore\n"
                             "Terminal=false\n"
                             "NoDisplay=true\n"
                             "X-GNOME-Autostart-enabled=true\n")
                             .arg(exec);

  // Only rewrite if missing or stale (e.g. the binary moved), to avoid churn.
  QFile f(path);
  if (f.open(QIODevice::ReadOnly))
  {
    const QString existing = QString::fromUtf8(f.readAll());
    f.close();
    if (existing == contents)
      return;
  }
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    QTextStream(&f) << contents;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
: QMainWindow(parent)
, m_bridges(/*enableJackProbe=*/true)
{
  setWindowTitle(tr("Audio Control"));

  if (!m_mixer.open())
  {
    auto* msg = new QLabel(tr("Could not open the ALSA mixer (\"default\").\n"
                              "Check that a sound card is present and alsa-utils is installed."),
                           this);
    msg->setAlignment(Qt::AlignCenter);
    msg->setMargin(24);
    setCentralWidget(msg);
    return;
  }

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->addWidget(buildMixerSection());
  layout->addWidget(buildRoutingSection());
  layout->addStretch(1);
  layout->addWidget(buildAppearanceBar()); // bottom-left appearance controls
  setCentralWidget(central);

  applyMeterThemeColors();

  connect(&m_mixer, &AlsaMixer::changed, this, &MainWindow::onMixerChanged);
  connect(&m_bridges, &BridgeManager::jackAvailabilityChanged, this, &MainWindow::onJackAvailabilityChanged);

  // Make the saved/default routing active if it isn't already (the bridge may
  // have been started at login and must be left running), and keep the login
  // autostart entry up to date.
  m_bridges.ensureActive();
  ensureAutostartEntry();
  onJackAvailabilityChanged(m_bridges.jackAvailable());
}

QWidget* MainWindow::buildMixerSection()
{
  auto* group = new QGroupBox(tr("Mixer"), this);
  auto* col = new QVBoxLayout(group);

  // One strip per element that exposes a volume (Master, Headphone, Speaker,
  // Mic, Mic Boost). Switch-only controls go in the routing section instead.
  for (const AlsaMixer::Element& e : m_mixer.elements())
  {
    if (!e.hasVolume)
      continue;
    auto* strip = new MixerStripWidget(&m_mixer, e, group);
    m_strips.push_back(strip);
    col->addWidget(strip);
  }

  m_meter = new LevelMeter(group);
  col->addSpacing(6);
  col->addWidget(new QLabel(tr("Output level"), group));
  col->addWidget(m_meter);

  return group;
}

QWidget* MainWindow::buildRoutingSection()
{
  auto* group = new QGroupBox(tr("Audio options && switches"), this);
  auto* col = new QVBoxLayout(group);

  // ---- 3-way routing radio (mutually exclusive bridges) ----
  auto* alsaRadio = new QRadioButton(tr("PA Bridge → ALSA (default)"), group);
  auto* pureRadio = new QRadioButton(tr("Pure ALSA (no bridge)"), group);
  m_jackRadio = new QRadioButton(tr("PA Bridge → JACK"), group);

  // Reflect what is actually active (a bridge may already be running from login)
  // without re-triggering setMode while we set up.
  switch (m_bridges.currentMode())
  {
    case BridgeManager::Mode::PulseToAlsa:
    {
      QSignalBlocker b(alsaRadio);
      alsaRadio->setChecked(true);
      break;
    }
    case BridgeManager::Mode::PureAlsa:
    {
      QSignalBlocker b(pureRadio);
      pureRadio->setChecked(true);
      break;
    }
    case BridgeManager::Mode::PulseToJack:
    {
      QSignalBlocker b(m_jackRadio);
      m_jackRadio->setChecked(true);
      break;
    }
  }

  auto* radioGroup = new QButtonGroup(this);
  radioGroup->addButton(alsaRadio);
  radioGroup->addButton(pureRadio);
  radioGroup->addButton(m_jackRadio);

  connect(alsaRadio, &QRadioButton::toggled, this, [this](bool on) {
    if (on)
      m_bridges.setMode(BridgeManager::Mode::PulseToAlsa);
  });
  connect(pureRadio, &QRadioButton::toggled, this, [this](bool on) {
    if (on)
      m_bridges.setMode(BridgeManager::Mode::PureAlsa);
  });
  connect(m_jackRadio, &QRadioButton::toggled, this, [this](bool on) {
    if (on)
      m_bridges.setMode(BridgeManager::Mode::PulseToJack);
  });

  col->addWidget(alsaRadio);
  col->addWidget(pureRadio);
  col->addWidget(m_jackRadio);

  // ---- Switch-only ALSA controls (Capture, IEC958) ----
  bool addedSep = false;
  for (const AlsaMixer::Element& e : m_mixer.elements())
  {
    if (e.hasVolume || !e.hasSwitch)
      continue;
    if (!addedSep)
    {
      col->addSpacing(6);
      addedSep = true;
    }
    auto* box = new QCheckBox(e.label, group);
    box->setChecked(m_mixer.switchOn(e));
    const AlsaMixer::Element captured = e;
    connect(box, &QCheckBox::toggled, this, [this, captured](bool on) { m_mixer.setSwitchOn(captured, on); });
    col->addWidget(box);
    m_switches.push_back({e, box});
  }

  return group;
}

QWidget* MainWindow::buildAppearanceBar()
{
  auto* bar = new QWidget(this);
  auto* row = new QHBoxLayout(bar);
  row->setContentsMargins(2, 0, 2, 0);

  // Accent colour picker, with a small colour swatch per entry.
  m_accentCombo = new QComboBox(bar);
  const Theme::Accent accents[] = {
    Theme::Accent::Green, Theme::Accent::Orange, Theme::Accent::Blue, Theme::Accent::Yellow};
  for (Theme::Accent a : accents)
  {
    QPixmap swatch(12, 12);
    swatch.fill(Qt::transparent);
    QPainter pp(&swatch);
    pp.setRenderHint(QPainter::Antialiasing, true);
    pp.setPen(Qt::NoPen);
    pp.setBrush(Theme::accentColor(a));
    pp.drawRoundedRect(swatch.rect(), 3, 3);
    pp.end();
    m_accentCombo->addItem(QIcon(swatch), Theme::accentName(a));
  }
  m_accentCombo->setCurrentIndex(static_cast<int>(Theme::savedAccent()));
  m_accentCombo->setToolTip(tr("Accent colour"));
  connect(m_accentCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onAppearanceChanged);
  row->addWidget(m_accentCombo);

  // Dark / light toggle (checked == light).
  m_modeToggle = new QToolButton(bar);
  m_modeToggle->setCheckable(true);
  m_modeToggle->setChecked(Theme::savedMode() == Theme::Mode::Light);
  m_modeToggle->setText(m_modeToggle->isChecked() ? QStringLiteral("☀ Light") : QStringLiteral("🌙 Dark"));
  m_modeToggle->setToolTip(tr("Toggle dark / light mode"));
  connect(m_modeToggle, &QToolButton::toggled, this, &MainWindow::onAppearanceChanged);
  row->addWidget(m_modeToggle);

  // Keep the controls grouped at the left edge.
  row->addStretch(1);

  return bar;
}

void MainWindow::applyMeterThemeColors()
{
  if (!m_meter)
    return;
  const auto mode = m_modeToggle && m_modeToggle->isChecked() ? Theme::Mode::Light : Theme::Mode::Dark;
  m_meter->setThemeColors(Theme::meterBackground(mode), Theme::meterTrack(mode));
}

void MainWindow::onAppearanceChanged()
{
  const auto mode = m_modeToggle->isChecked() ? Theme::Mode::Light : Theme::Mode::Dark;
  const auto accent = static_cast<Theme::Accent>(m_accentCombo->currentIndex());

  m_modeToggle->setText(mode == Theme::Mode::Light ? QStringLiteral("☀ Light") : QStringLiteral("🌙 Dark"));

  Theme::apply(mode, accent);
  Theme::save(mode, accent);
  applyMeterThemeColors();
}

void MainWindow::onMixerChanged()
{
  // External change (amixer, another app): re-sync every widget.
  for (MixerStripWidget* strip : m_strips)
    strip->refresh();
  for (const SwitchBox& s : m_switches)
  {
    QSignalBlocker block(s.box);
    s.box->setChecked(m_mixer.switchOn(s.element));
  }
}

void MainWindow::onJackAvailabilityChanged(bool available)
{
  if (!m_jackRadio)
    return;
  m_jackRadio->setEnabled(available);
  m_jackRadio->setToolTip(available ? tr("Route PulseAudio-compat audio to JACK ports")
                                    : tr("Start jackd to enable JACK routing"));
}
