#include "copyoverlay.h"
#include "ui_copyoverlay.h"

#include "components/actionmanager/actionmanager.h"
#include "shortcutbuilder.h"

CopyOverlay::CopyOverlay(FloatingWidgetContainer *parent) : OverlayWidget(parent), m_ui(new Ui::CopyOverlay) {
    m_ui->setupUi(this);
    hide();
    setFadeEnabled(true);

    m_ui->closeButton->setIconPath(":/res/icons/common/overlay/close-dim16.png");
    m_ui->headerIcon->setIconPath(":/res/icons/common/overlay/copy16.png");
    m_ui->headerLabel->setText(tr("Copy to..."));
    m_mode = OVERLAY_COPY;

    createShortcuts();

    m_paths = settings->savedPaths();
    if(m_paths.count() < m_maxPathCount)
        createDefaultPaths();
    createPathWidgets();

    setAcceptKeyboardFocus(true);

    if(parent)
        setContainerSize(parent->size());

    readSettings();
    connect(settings, &Settings::settingsChanged, this, &CopyOverlay::readSettings);
}

CopyOverlay::~CopyOverlay() {
    delete m_ui;
}

void CopyOverlay::show() {
    OverlayWidget::show();
    setFocus();
}

void CopyOverlay::hide() {
    OverlayWidget::hide();
}

void CopyOverlay::setDialogMode(CopyOverlayMode _mode) {
    m_mode = _mode;
    if(m_mode == OVERLAY_COPY) {
        m_ui->headerIcon->setIconPath(":/res/icons/common/overlay/copy16.png");
        m_ui->headerLabel->setText(tr("Copy to..."));
    } else {
        m_ui->headerIcon->setIconPath(":/res/icons/common/overlay/move16.png");
        m_ui->headerLabel->setText(tr("Move to..."));
    }
}

CopyOverlayMode CopyOverlay::operationMode() {
    return m_mode;
}

void CopyOverlay::removePathWidgets() {
    for(int i = 0; i < m_pathWidgets.count(); i++) {
        QWidget *tmp = m_pathWidgets.at(i);
        m_ui->pathSelectorsLayout->removeWidget(tmp);
        delete tmp;
    }
    m_pathWidgets.clear();
}

void CopyOverlay::createPathWidgets() {
    removePathWidgets();
    int count = (m_paths.length() > m_maxPathCount) ? m_maxPathCount : m_paths.length();
    for(int i = 0; i < count; i++) {
        PathSelectorMenuItem *item = new PathSelectorMenuItem(this);
        item->setDirectory(m_paths.at(i));
        item->setShortcutText(m_shortcuts.key(i));
        connect(item, &PathSelectorMenuItem::directorySelected, this, &CopyOverlay::requestFileOperation);
        m_pathWidgets.append(item);
        m_ui->pathSelectorsLayout->addWidget(item);
    }
}

void CopyOverlay::createShortcuts() {
    for(int i = 0; i < m_maxPathCount; i++)
        m_shortcuts.insert(QString::number(i + 1), i);
}

void CopyOverlay::requestFileOperation(const QString &path) {
    if(m_mode == OVERLAY_COPY)
        emit copyRequested(path);
    else
        emit moveRequested(path);
}

void CopyOverlay::readSettings() {
    // don't interfere with the main panel
    if(settings->panelEnabled() && settings->panelPosition() == PanelPosition::PANEL_BOTTOM) {
        setPosition(FloatingWidgetPosition::TOPLEFT);
    } else {
        setPosition(FloatingWidgetPosition::BOTTOMLEFT);
    }
    update();
}

// for some reason, duplicate folders may appear in the configuration
// we remove duplicate directories
void CopyOverlay::saveSettings() {
    m_paths.clear();
    QStringList temp;
    for(int i = 0; i < m_pathWidgets.count(); i++) {
        QString path = m_pathWidgets.at(i)->path();
        if(!path.isEmpty()) {
            if(!temp.contains(path)) {
                temp << path;
                m_paths << m_pathWidgets.at(i)->directory();
            }
        }
    }
    settings->setSavedPaths(m_paths);
}

void CopyOverlay::createDefaultPaths() {
    QString home = QDir::homePath();
    if(m_paths.count() < 1 || m_paths.at(0).isEmpty() || m_paths.at(0)[0] == '@') {
        m_paths.clear();
        m_paths << home;
    }
    if(m_paths.count() == 1 && m_paths.at(0) == home) {
        QDir dir(home);
        foreach(QFileInfo mfi, dir.entryInfoList()) {
            if(m_paths.count() >= m_maxPathCount) {
                break;
            }
            if(mfi.isFile()) {
                continue;
            } else {
                if(mfi.fileName() == "." ||
                   mfi.fileName() == ".."
                   // hide directory
                   || mfi.fileName().startsWith('.')
                   // windows system directory
                   || mfi.fileName() == "3D Objects" || mfi.fileName() == "Contacts" || mfi.fileName() == "Favorites" ||
                   mfi.fileName() == "Links" || mfi.fileName() == "Saved Games" || mfi.fileName() == "Searches") {
                    continue;
                }
                QString qpath(home + "/" + mfi.fileName());
                QFileInfo qinfo(qpath);
                if(qinfo.permission(QFile::WriteUser | QFile::ReadGroup)) {
                    m_paths << qpath;
                }
            }
        }
    }
}

// block native tab-switching so we can use it in shortcuts
bool CopyOverlay::focusNextPrevChild(bool next) {
    Q_UNUSED(next)
    return false;
}

// Same veto law as the save prompt, plus the digit keys: 1-9 are this
// surface's per-destination hotkeys, and the global table binds 1/2/3 to the
// fit modes — without the veto a digit would refit the view instead of
// addressing a copy target.
bool CopyOverlay::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const bool digit = ke->key() >= Qt::Key_1 && ke->key() <= Qt::Key_9 &&
                           (ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if(actionManager->keyNamesSubjectChangingAction(ke) || digit)
            event->accept();
    }
    return OverlayWidget::event(event);
}

void CopyOverlay::keyPressEvent(QKeyEvent *event) {
    event->accept();
    // the surface owns Esc: dismiss without copying — never forward it to the
    // action system, where "Esc" maps to closeFullScreenOrExit (app exit)
    if(event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    // a bare Enter is no answer here — the digits are the answers, and
    // yielding Enter would toggle the folder view behind the menu
    if(event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        return;
    QString key = actionManager->keyForNativeScancode(event->nativeScanCode());
    if(m_shortcuts.contains(key)) {
        requestFileOperation(m_pathWidgets.at(m_shortcuts[key])->directory());
        return;
    }
    // the menu names a target for a specific file: walking to another image
    // while it is open would leave the user unable to say which file they are
    // copying. Those keys are consumed, not yielded; zoom/fullscreen still are.
    if(actionManager->keyNamesSubjectChangingAction(event))
        return; // the walk was vetoed back here — consumed, same as the answer keys
}

// Clicking outside the menu is the same answer as Esc: dismiss without
// copying. widgetAt tells us whether the click actually landed on this
// widget — a click on a path item is inside it (the item handles it), a
// click on the image behind is not.
void CopyOverlay::mousePressEvent(QMouseEvent *event) {
    event->accept();
    if(qApp->widgetAt(mapToGlobal(event->pos())) == this)
        hide();
}
