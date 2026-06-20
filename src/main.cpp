// main.cpp — Audio-Gui entry point.
//
// Normal launch shows the control window. `audio-gui --restore` is a headless
// path used by the XDG autostart entry at login: it re-applies the saved routing
// (starting the chosen bridge detached) and exits, so audio works before — and
// without — the GUI ever being opened.
#include <QApplication>

#include <cstring>

#include "BridgeManager.h"
#include "MainWindow.h"
#include "Theme.h"

int main(int argc, char* argv[])
{
  // Identity for QSettings (persisted routing choice): ~/.config/AudioGui/.
  QCoreApplication::setOrganizationName(QStringLiteral("AudioGui"));
  QCoreApplication::setApplicationName(QStringLiteral("AudioGui"));

  bool restore = false;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--restore") == 0)
      restore = true;

  if (restore)
  {
    // No GUI, no event loop: apply the saved mode (bridge is detached) and quit.
    QCoreApplication app(argc, argv);
    BridgeManager mgr(/*enableJackProbe=*/false);
    mgr.restoreSavedMode();
    return 0;
  }

  QApplication app(argc, argv);
  app.setApplicationDisplayName(QStringLiteral("Audio Control"));

  // Self-contained look (Fusion + stylesheet); ignores the user's system theme.
  Theme::apply(Theme::savedMode(), Theme::savedAccent());

  MainWindow window;
  window.resize(440, 460);
  window.show();

  return app.exec();
}
