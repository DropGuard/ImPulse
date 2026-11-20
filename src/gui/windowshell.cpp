#include "gui/windowshell.h"

#include <QApplication>
#include <QWindow>

#include "gui/mainwindow.h"
#include "components/actionmanager/actionmanager.h"
#include "settings.h"

#ifdef USE_KDE_BLUR
#include <KWindowEffects>
#endif

WindowShell::WindowShell(MW *windowArg)
    : QObject(windowArg), m_window(windowArg), m_currentDisplay(0), m_maximized(false) {
    m_window->installEventFilter(this);
    m_geometryChangeTimer.setSingleShot(true);
    m_geometryChangeTimer.setInterval(30);
    connect(&m_geometryChangeTimer, &QTimer::timeout, this, &WindowShell::saveWindowGeometry);
    connect(&m_geometryChangeTimer, &QTimer::timeout, this, &WindowShell::updateCurrentDisplay);
    m_currentDisplay = settings->lastDisplay();
    m_maximized = settings->maximizedWindow();
}

bool WindowShell::eventFilter(QObject *watched, QEvent *event) {
    // only save maximized state if we are already visible
    // this filters out events while the window is still being set up
    if(event->type() == QEvent::WindowStateChange && m_window->isVisible() && !m_window->isFullScreen())
        m_maximized = m_window->isMaximized();
    if(event->type() == QEvent::Move || event->type() == QEvent::Resize)
        m_geometryChangeTimer.start();
    return QObject::eventFilter(watched, event);
}

void WindowShell::saveWindowGeometry() {
    if(m_window->windowState() == Qt::WindowNoState)
        settings->setWindowGeometry(m_window->geometry());
    settings->setMaximizedWindow(m_maximized);
}

// does not apply fullscreen; window size / maximized state only
void WindowShell::restoreWindowGeometry() {
    m_window->setGeometry(settings->windowGeometry());
    if(settings->maximizedWindow())
        m_window->setWindowState(Qt::WindowMaximized);
    updateCurrentDisplay();
}

void WindowShell::updateCurrentDisplay() {
    m_currentDisplay = qApp->screens().indexOf(m_window->window()->screen());
}

void WindowShell::saveCurrentDisplay() {
    settings->setLastDisplay(qApp->screens().indexOf(m_window->window()->screen()));
}

void WindowShell::preShowResize(QSize sz) {
    auto screens = qApp->screens();
    if(m_window->windowState() != Qt::WindowNoState || !screens.count() || screens.count() <= m_currentDisplay)
        return;
    int decorationSize = m_window->frameGeometry().height() - m_window->height();
    float maxSzMulti = settings->autoResizeLimit() / 100.f;
    QRect availableGeom = screens.at(m_currentDisplay)->availableGeometry();
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

    if(m_window->isVisible())
        m_window->setGeometry(newGeom);
    else // setGeometry wont work on hidden windows, so we just save for it to be restored later
        settings->setWindowGeometry(newGeom);
    qApp->processEvents(); // not needed anymore with patched qt?
}

void WindowShell::showDefault() {
    if(!m_window->isVisible()) {
        if(settings->fullscreenMode())
            showFullScreen();
        else
            showWindowed();
    }
}

void WindowShell::triggerFullScreen() {
    if(!m_window->isFullScreen()) {
        showFullScreen();
    } else {
        showWindowed();
    }
}

void WindowShell::showFullScreen() {
    // do not save immediately on application start
    if(!m_window->isHidden())
        saveWindowGeometry();
    auto screens = qApp->screens();
    // todo: why check the screen again?
    int _currentDisplay = screens.indexOf(m_window->window()->screen());
    // move to target screen
    if(screens.count() > m_currentDisplay && m_currentDisplay != _currentDisplay) {
        m_window->move(screens.at(m_currentDisplay)->geometry().x(), screens.at(m_currentDisplay)->geometry().y());
    }
    m_window->showFullScreen();
    // try to repaint sooner
    qApp->processEvents();
    emit fullscreenStateChanged(true);
}

void WindowShell::showWindowed() {
    if(m_window->isFullScreen())
        m_window->showNormal();
    restoreWindowGeometry();
    m_window->show();
    // try to repaint sooner
    qApp->processEvents();
    emit fullscreenStateChanged(false);
}

void WindowShell::closeFullScreenOrExit() {
    if(m_window->isFullScreen()) {
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
