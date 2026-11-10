#include "gui/windowshell.h"

#include <QApplication>
#include <QWindow>

#include "gui/mainwindow.h"
#include "components/actionmanager/actionmanager.h"
#include "settings.h"

#ifdef USE_KDE_BLUR
#include <KWindowEffects>
#endif

WindowShell::WindowShell(MW *windowArg) : QObject(windowArg), window(windowArg), currentDisplay(0), maximized(false) {
    window->installEventFilter(this);
    geometryChangeTimer.setSingleShot(true);
    geometryChangeTimer.setInterval(30);
    connect(&geometryChangeTimer, &QTimer::timeout, this, &WindowShell::saveWindowGeometry);
    connect(&geometryChangeTimer, &QTimer::timeout, this, &WindowShell::updateCurrentDisplay);
    currentDisplay = settings->lastDisplay();
    maximized = settings->maximizedWindow();
}

bool WindowShell::eventFilter(QObject *watched, QEvent *event) {
    // only save maximized state if we are already visible
    // this filters out events while the window is still being set up
    if(event->type() == QEvent::WindowStateChange && window->isVisible() && !window->isFullScreen())
        maximized = window->isMaximized();
    if(event->type() == QEvent::Move || event->type() == QEvent::Resize)
        geometryChangeTimer.start();
    return QObject::eventFilter(watched, event);
}

void WindowShell::saveWindowGeometry() {
    if(window->windowState() == Qt::WindowNoState)
        settings->setWindowGeometry(window->geometry());
    settings->setMaximizedWindow(maximized);
}

// does not apply fullscreen; window size / maximized state only
void WindowShell::restoreWindowGeometry() {
    window->setGeometry(settings->windowGeometry());
    if(settings->maximizedWindow())
        window->setWindowState(Qt::WindowMaximized);
    updateCurrentDisplay();
}

void WindowShell::updateCurrentDisplay() {
    currentDisplay = qApp->screens().indexOf(window->window()->screen());
}

void WindowShell::saveCurrentDisplay() {
    settings->setLastDisplay(qApp->screens().indexOf(window->window()->screen()));
}

void WindowShell::preShowResize(QSize sz) {
    auto screens = qApp->screens();
    if(window->windowState() != Qt::WindowNoState || !screens.count() || screens.count() <= currentDisplay)
        return;
    int decorationSize = window->frameGeometry().height() - window->height();
    float maxSzMulti = settings->autoResizeLimit() / 100.f;
    QRect availableGeom = screens.at(currentDisplay)->availableGeometry();
    QSize maxSz = availableGeom.size() * maxSzMulti;
    maxSz.setHeight(maxSz.height() - decorationSize);
    if(!sz.isEmpty()) {
        if(sz.width() > maxSz.width() || sz.height() > maxSz.height())
            sz.scale(maxSz, Qt::KeepAspectRatio);
    } else {
        sz = maxSz;
    }
    QRect newGeom(0, 0, sz.width(), sz.height());
    newGeom.moveCenter(availableGeom.center());
    newGeom.translate(0, decorationSize / 2);

    if(window->isVisible())
        window->setGeometry(newGeom);
    else // setGeometry wont work on hidden windows, so we just save for it to be restored later
        settings->setWindowGeometry(newGeom);
    qApp->processEvents(); // not needed anymore with patched qt?
}

void WindowShell::showDefault() {
    if(!window->isVisible()) {
        if(settings->fullscreenMode())
            showFullScreen();
        else
            showWindowed();
    }
}

void WindowShell::triggerFullScreen() {
    if(!window->isFullScreen()) {
        showFullScreen();
    } else {
        showWindowed();
    }
}

void WindowShell::showFullScreen() {
    // do not save immediately on application start
    if(!window->isHidden())
        saveWindowGeometry();
    auto screens = qApp->screens();
    // todo: why check the screen again?
    int _currentDisplay = screens.indexOf(window->window()->screen());
    // move to target screen
    if(screens.count() > currentDisplay && currentDisplay != _currentDisplay) {
        window->move(screens.at(currentDisplay)->geometry().x(), screens.at(currentDisplay)->geometry().y());
    }
    window->showFullScreen();
    // try to repaint sooner
    qApp->processEvents();
    emit fullscreenStateChanged(true);
}

void WindowShell::showWindowed() {
    if(window->isFullScreen())
        window->showNormal();
    restoreWindowGeometry();
    window->show();
    // try to repaint sooner
    qApp->processEvents();
    emit fullscreenStateChanged(false);
}

void WindowShell::closeFullScreenOrExit() {
    if(window->isFullScreen()) {
        showWindowed();
    } else {
        actionManager->invokeAction("exit");
    }
}

// todo: remove/rename?
void WindowShell::applyWindowedBackground() {
#ifdef USE_KDE_BLUR
    QWindow *windowHandle = window->windowHandle();
    if(windowHandle) {
        if(settings->backgroundOpacity() == 1.0)
            KWindowEffects::enableBlurBehind(windowHandle, false);
        else
            KWindowEffects::enableBlurBehind(windowHandle, settings->blurBackground());
    }
#endif
}

void WindowShell::applyFullscreenBackground() {
#ifdef USE_KDE_BLUR
    QWindow *windowHandle = window->windowHandle();
    if(windowHandle)
        KWindowEffects::enableBlurBehind(windowHandle, false);
#endif
}
