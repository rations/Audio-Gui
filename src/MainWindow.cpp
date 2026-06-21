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
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "AlsaDevices.h"
#include "LevelMeter.h"
#include "MixerStripWidget.h"
#include "Theme.h"

namespace
{
constexpr int kHotplugPollMs = 3000;
} // namespace

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

  // Make the saved/default routing active first. This also bootstraps a usable
  // ALSA "default" (~/.asoundrc) when the user has none — and the mixer below
  // opens "default", so it must exist first. Otherwise a missing default makes
  // m_mixer.open() fail and we bail out (below) before ever writing it: the one
  // case that needs regeneration is the one that would skip it.
  m_bridges.ensureActive();

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

  // Build sections before placing them. Routing is built before Devices so the
  // switches container (m_switchesLayout) exists when populating device controls.
  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  QWidget* mixerSection = buildMixerSection();
  QWidget* routingSection = buildRoutingSection();
  QWidget* devicesSection = buildDevicesSection();
  layout->addWidget(mixerSection);
  layout->addWidget(devicesSection);
  layout->addWidget(routingSection);
  layout->addStretch(1);
  layout->addWidget(buildAppearanceBar()); // bottom-left appearance controls
  setCentralWidget(central);

  // Open the mixer on the card backing the combo's current selection (which the
  // devices section already resolved, falling back to an available card if the
  // saved device is currently absent) and fill the controls.
  reopenMixerForDevice(m_deviceCombo->currentData().toString());
  applyMeterThemeColors();

  connect(&m_mixer, &AlsaMixer::changed, this, &MainWindow::onMixerChanged);
  connect(&m_bridges, &BridgeManager::jackAvailabilityChanged, this, &MainWindow::onJackAvailabilityChanged);
  connect(&m_bridges, &BridgeManager::modeChanged, this, [this](BridgeManager::Mode) { updateDeviceComboEnabled(); });

  // Re-scan cards periodically so USB/HDMI plug/unplug updates the list live —
  // no udev, no daemon, all in-process.
  m_hotplugTimer = new QTimer(this);
  m_hotplugTimer->setInterval(kHotplugPollMs);
  connect(m_hotplugTimer, &QTimer::timeout, this, &MainWindow::refreshDevices);
  m_hotplugTimer->start();

  // Routing was already made active at the top of the constructor (before the
  // mixer opened "default"). Keep the login autostart entry up to date.
  ensureAutostartEntry();
  onJackAvailabilityChanged(m_bridges.jackAvailable());
  updateDeviceComboEnabled();
}

QWidget* MainWindow::buildMixerSection()
{
  auto* group = new QGroupBox(tr("Mixer"), this);
  auto* col = new QVBoxLayout(group);

  // Volume strips are rebuilt into this layout whenever the output device (and
  // thus the card) changes — see populateMixerControls().
  m_stripsLayout = new QVBoxLayout();
  col->addLayout(m_stripsLayout);

  m_mixerPlaceholder = new QLabel(tr("No mixer controls for this output."), group);
  m_mixerPlaceholder->setAlignment(Qt::AlignCenter);
  m_mixerPlaceholder->hide();
  m_stripsLayout->addWidget(m_mixerPlaceholder);

  m_meter = new LevelMeter(group);
  col->addSpacing(6);
  col->addWidget(new QLabel(tr("Output level"), group));
  col->addWidget(m_meter);

  return group;
}

QWidget* MainWindow::buildDevicesSection()
{
  auto* group = new QGroupBox(tr("Output device"), this);
  auto* col = new QVBoxLayout(group);

  m_deviceCombo = new QComboBox(group);
  m_deviceCombo->setToolTip(tr("Switch the audio output device live"));
  col->addWidget(m_deviceCombo);

  // Fill the list and select the persisted device.
  refreshDevices();

  connect(m_deviceCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceSelected);
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
  // These are card-specific, so they are (re)built into this layout by
  // populateMixerControls() whenever the output device changes.
  col->addSpacing(6);
  m_switchesLayout = new QVBoxLayout();
  col->addLayout(m_switchesLayout);

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

void MainWindow::populateMixerControls(bool suppressControls)
{
  // Tear down the previous card's widgets (the placeholder is kept and toggled).
  qDeleteAll(m_strips);
  m_strips.clear();
  for (const SwitchBox& s : m_switches)
    delete s.box;
  m_switches.clear();

  // USB interfaces (and any other suppressed card) show only the placeholder:
  // their software controls don't drive the hardware, so a slider would mislead.
  if (suppressControls)
  {
    m_mixerPlaceholder->setVisible(true);
    return;
  }

  QWidget* stripParent = m_stripsLayout->parentWidget();
  QWidget* switchParent = m_switchesLayout->parentWidget();

  // One strip per element that exposes a volume (Master, Headphone, Speaker, ...).
  bool anyVolume = false;
  for (const AlsaMixer::Element& e : m_mixer.elements())
  {
    if (!e.hasVolume)
      continue;
    auto* strip = new MixerStripWidget(&m_mixer, e, stripParent);
    m_strips.push_back(strip);
    m_stripsLayout->addWidget(strip);
    anyVolume = true;
  }
  m_mixerPlaceholder->setVisible(!anyVolume);

  // Switch-only controls (Capture, IEC958) for this card.
  for (const AlsaMixer::Element& e : m_mixer.elements())
  {
    if (e.hasVolume || !e.hasSwitch)
      continue;
    auto* box = new QCheckBox(e.label, switchParent);
    box->setChecked(m_mixer.switchOn(e));
    const AlsaMixer::Element captured = e;
    connect(box, &QCheckBox::toggled, this, [this, captured](bool on) { m_mixer.setSwitchOn(captured, on); });
    m_switchesLayout->addWidget(box);
    m_switches.push_back({e, box});
  }
}

void MainWindow::reopenMixerForDevice(const QString& token)
{
  // Internal / empty -> the ALSA "default" mixer; a specific card -> its controls.
  const QString cardId = AlsaDevices::cardIdFromToken(token);
  const QString card = cardId.isEmpty() ? QStringLiteral("default") : QStringLiteral("hw:CARD=%1").arg(cardId);

  // USB interfaces expose only nominal capture controls (gain is set by hardware
  // knobs), so their sliders do nothing useful — show the placeholder like HDMI.
  const QVector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();
  const AlsaDevices::OutputDevice* d = AlsaDevices::findByToken(devices, token);
  const bool suppressControls = d && d->category == AlsaDevices::Category::Usb;

  m_mixer.reopen(card); // on failure the element list is empty -> placeholder shows
  populateMixerControls(suppressControls);
}

void MainWindow::onDeviceSelected(int index)
{
  if (index < 0 || !m_deviceCombo)
    return;
  const QString token = m_deviceCombo->itemData(index).toString();
  m_bridges.setDevice(token); // restarts the bridge on the new device (no-op if unchanged)
  reopenMixerForDevice(token); // mixer follows the device
}

void MainWindow::refreshDevices()
{
  const QVector<AlsaDevices::OutputDevice> devices = AlsaDevices::enumerateOutputs();

  // Cheap change detection: only rebuild when the set of devices actually changed.
  auto signature = [](const QVector<AlsaDevices::OutputDevice>& v) {
    QStringList s;
    for (const AlsaDevices::OutputDevice& d : v)
      s << d.cardId + QLatin1Char(':') + QString::number(d.pcmIndex);
    return s;
  };
  const bool initialBuild = m_deviceCombo->count() == 0;
  if (!initialBuild && signature(devices) == signature(m_devices))
    return;
  m_devices = devices;

  // Preserve the user's selection across the rebuild; on first fill use the
  // persisted choice. Internal devices are stored as "" (== ALSA "default").
  const QString want = initialBuild ? m_bridges.currentDevice() : m_deviceCombo->currentData().toString();

  QSignalBlocker block(m_deviceCombo);
  m_deviceCombo->clear();
  int wantIndex = -1;
  int internalIndex = -1;
  for (const AlsaDevices::OutputDevice& d : devices)
  {
    const QString token = AlsaDevices::tokenFor(d);
    m_deviceCombo->addItem(d.displayName, token);
    const int idx = m_deviceCombo->count() - 1;
    if (token == want && wantIndex < 0)
      wantIndex = idx;
    if (internalIndex < 0 && d.category == AlsaDevices::Category::Internal)
      internalIndex = idx;
  }

  const int select = wantIndex >= 0 ? wantIndex : (internalIndex >= 0 ? internalIndex : 0);
  if (m_deviceCombo->count() > 0)
    m_deviceCombo->setCurrentIndex(select);

  // On a live refresh (not the initial build): if the device that was selected
  // vanished (unplug), switch the running bridge + mixer to the new selection so
  // audio keeps playing. On the initial build we leave the saved preference
  // intact — the bridge resolves a missing device to "default" on its own, so a
  // device merely absent at startup is not forgotten.
  const QString chosen = m_deviceCombo->currentData().toString();
  if (!initialBuild && m_deviceCombo->count() > 0 && chosen != m_bridges.currentDevice())
  {
    m_bridges.setDevice(chosen);
    reopenMixerForDevice(chosen);
  }
}

void MainWindow::updateDeviceComboEnabled()
{
  if (!m_deviceCombo)
    return;
  // PA→ALSA switches live; Pure ALSA repoints ALSA's default (applies on app
  // restart). Only PA→JACK ignores the device (audio routes through jackd).
  const BridgeManager::Mode mode = m_bridges.currentMode();
  switch (mode)
  {
    case BridgeManager::Mode::PulseToAlsa:
      m_deviceCombo->setEnabled(true);
      m_deviceCombo->setToolTip(tr("Switch the audio output device live"));
      break;
    case BridgeManager::Mode::PureAlsa:
      m_deviceCombo->setEnabled(true);
      m_deviceCombo->setToolTip(tr("Set the default ALSA output device (applies to apps started afterwards)"));
      break;
    case BridgeManager::Mode::PulseToJack:
      m_deviceCombo->setEnabled(false);
      m_deviceCombo->setToolTip(tr("Device selection does not apply when routing to JACK"));
      break;
  }
}
