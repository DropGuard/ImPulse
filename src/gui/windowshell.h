#pragma once

// Window geometry / fullscreen / display-target state, extracted from MW
// (the strangler rewrite of mainwindow.cpp). MW stays the widget and the
// composition root; this class owns everything that outlives a mode switch:
// the saved geometry, the maximized cache, the target display, and the
// debounced "window moved/resized" persistence timer.
//
// It observes the window as an event filter (WindowStateChange / Move /
// Resize) — observation only, never consumption, so QWidget's own handling
// proceeds untouched.

#include <QObject>
#include <QTimer>
#include <QSize>

class MW;

class WindowShell : public QObject {
    Q_OBJECT
  public:
    explicit WindowShell(MW *window);
    // reapplies the persisted window state (size / maximized / target screen);
    // called by the MW constructor, never applies fullscreen
    void restoreWindowGeometry();
    void preShowResize(QSize sz);
    void showDefault();
    void triggerFullScreen();
    void showFullScreen();
    void showWindowed();
    void saveWindowGeometry();
    void saveCurrentDisplay();
    // quit fullscreen or exit the program
    void closeFullScreenOrExit();
    void applyWindowedBackground();
    void applyFullscreenBackground();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  signals:
    void fullscreenStateChanged(bool);

  private:
    void updateCurrentDisplay();

    MW *window;
    QTimer geometryChangeTimer;
    int currentDisplay;
    bool maximized;
};
