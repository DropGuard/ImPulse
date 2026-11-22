#include "contextmenu.h"
#include "ui_contextmenu.h"

#include <QStyle>

#include "components/actionmanager/actionmanager.h"

ContextMenu::ContextMenu(QWidget *parent) : QWidget(parent), m_ui(new Ui::ContextMenu) {
    m_ui->setupUi(this);
    setWindowFlags(Qt::Popup);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoMousePropagation, true);
    this->hide();

    // -------------------------------------------------------------------------
    // setup actions
    // top zoom buttons
    m_ui->zoomIn->setAction("zoomIn");
    m_ui->zoomIn->setIconPath(":/res/icons/common/buttons/contextmenu/zoom-in18.png");
    m_ui->zoomIn->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->zoomOut->setAction("zoomOut");
    m_ui->zoomOut->setIconPath(":/res/icons/common/buttons/contextmenu/zoom-out18.png");
    m_ui->zoomOut->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->zoomOriginal->setAction("fitNormal");
    m_ui->zoomOriginal->setIconPath(":/res/icons/common/buttons/contextmenu/zoom-original18.png");
    m_ui->zoomOriginal->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->fitWidth->setAction("fitWidth");
    m_ui->fitWidth->setIconPath(":/res/icons/common/buttons/contextmenu/fit-width18.png");
    m_ui->fitWidth->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->fitWindow->setAction("fitWindow");
    m_ui->fitWindow->setIconPath(":/res/icons/common/buttons/contextmenu/fit-window18.png");
    m_ui->fitWindow->setTriggerMode(TriggerMode::PressTrigger);
    // -------------------------------------------------------------------------
    // transform buttons
    m_ui->rotateLeft->setAction("rotateLeft");
    m_ui->rotateLeft->setIconPath(":/res/icons/common/menuitem/rotate-left16.png");
    m_ui->rotateLeft->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->rotateRight->setAction("rotateRight");
    m_ui->rotateRight->setIconPath(":/res/icons/common/menuitem/rotate-right16.png");
    m_ui->rotateRight->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->flipH->setAction("flipH");
    m_ui->flipH->setIconPath(":/res/icons/common/menuitem/flip-h16.png");
    m_ui->flipH->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->flipV->setAction("flipV");
    m_ui->flipV->setIconPath(":/res/icons/common/menuitem/flip-v16.png");
    m_ui->flipV->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->crop->setAction("crop");
    m_ui->crop->setIconPath(":/res/icons/common/menuitem/image-crop16.png");
    m_ui->crop->setTriggerMode(TriggerMode::PressTrigger);
    m_ui->resize->setAction("resize");
    m_ui->resize->setIconPath(":/res/icons/common/menuitem/resize16.png");
    m_ui->resize->setTriggerMode(TriggerMode::PressTrigger);
    // -------------------------------------------------------------------------
    //  entries
    m_ui->print->setAction("print");
    m_ui->print->setText(tr("Print"));
    m_ui->print->setIconPath(":/res/icons/common/menuitem/print16.png");
    m_ui->setWallpaper->setAction("setWallpaper");
    m_ui->setWallpaper->setText(tr("Set as wallpaper"));
    m_ui->setWallpaper->setIconPath(":/res/icons/common/menuitem/wallpaper16.png");

    m_ui->copy->setAction("copyFile");
    m_ui->copy->setText(tr("Quick copy"));
    m_ui->copy->setIconPath(":/res/icons/common/menuitem/copy16.png");

    m_ui->move->setAction("moveFile");
    m_ui->move->setText(tr("Quick move"));
    m_ui->move->setIconPath(":/res/icons/common/menuitem/move16.png");

    m_ui->trash->setAction("moveToTrash");
    m_ui->trash->setText(tr("Move to trash"));
    m_ui->trash->setIconPath(":/res/icons/common/menuitem/trash16.png");
    // -------------------------------------------------------------------------
    m_ui->open->setAction("open");
    m_ui->open->setText(tr("Open"));
    m_ui->open->setIconPath(":/res/icons/common/menuitem/open16.png");

    m_ui->recoverEdits->setAction("recoverEdits");
    m_ui->recoverEdits->setText(tr("Recover edits"));
    m_ui->recoverEdits->setIconPath(":/res/icons/common/menuitem/open16.png");

    m_ui->folderView->setAction("folderView");
    m_ui->folderView->setText(tr("Folder View"));
    m_ui->folderView->setIconPath(":/res/icons/common/menuitem/folderview16.png");

    m_ui->slideshow->setAction("toggleSlideshow");
    m_ui->slideshow->setText(tr("Slideshow"));
    m_ui->slideshow->setIconPath(":/res/icons/common/menuitem/slideshow16.png");

    m_ui->settings->setAction("openSettings");
    m_ui->settings->setText(tr("Settings"));
    m_ui->settings->setIconPath(":/res/icons/common/menuitem/settings16.png");
    // -------------------------------------------------------------------------
    m_ui->openWith->setText(tr("Open with..."));
    m_ui->openWith->setIconPath(":/res/icons/common/menuitem/run16.png");
    m_ui->openWith->setPassthroughClicks(false);
    connect(m_ui->openWith, &ContextMenuItem::pressed, this, &ContextMenu::switchToScriptsPage);
    // -------------------------------------------------------------------------
    m_ui->showLocation->setAction("showInDirectory");
    m_ui->showLocation->setText(tr("Show in folder"));
    m_ui->showLocation->setIconPath(":/res/icons/common/menuitem/folder16.png");
    // -------------------------------------------------------------------------
    // force resize to fit new menuitem width
    this->adjustSize();

    // Scripts page
    // -------------------------------------------------------------------------
    m_ui->backButton->setText(tr("Back"));
    m_ui->backButton->setIconPath(":/res/icons/common/menuitem/back16.png");
    m_ui->backButton->setPassthroughClicks(false);
    m_ui->scriptSetupButton->setText(tr("Configure menu"));
    m_ui->scriptSetupButton->setIconPath(":/res/icons/common/menuitem/settings16.png");
    connect(m_ui->backButton, &ContextMenuItem::pressed, this, &ContextMenu::switchToMainPage);
    connect(m_ui->scriptSetupButton, &ContextMenuItem::pressed, this, &ContextMenu::showScriptSettings);
    fillOpenWithMenu();
}

ContextMenu::~ContextMenu() {
    delete m_ui;
}

// child-add order matches the visual order closely enough for keyboard
// navigation; hidden and disabled entries are skipped so the selection
// never lands on a dead row
QList<ContextMenuItem *> ContextMenu::visibleItems() const {
    QList<ContextMenuItem *> items;
    const auto children =
        m_ui->stackedWidget->currentWidget()->findChildren<ContextMenuItem *>(QString(), Qt::FindDirectChildrenOnly);
    for(auto *item : children)
        if(item->isVisible() && item->isEnabled())
            items.append(item);
    return items;
}

void ContextMenu::applySelection() {
    const auto items = visibleItems();
    for(int i = 0; i < items.size(); ++i) {
        bool selected = (i == m_selectedIndex);
        if(items[i]->property("selected").toBool() != selected) {
            items[i]->setProperty("selected", selected);
            items[i]->style()->unpolish(items[i]);
            items[i]->style()->polish(items[i]);
            items[i]->update();
        }
    }
}

void ContextMenu::clearSelection() {
    m_selectedIndex = -1;
    // every child, visible or not: a stale property from a previous open
    // must not resurrect as a phantom highlight
    const auto children =
        m_ui->stackedWidget->currentWidget()->findChildren<ContextMenuItem *>(QString(), Qt::FindDirectChildrenOnly);
    for(auto *item : children) {
        if(item->property("selected").isValid()) {
            item->setProperty("selected", false);
            item->style()->unpolish(item);
            item->style()->polish(item);
            item->update();
        }
    }
}

void ContextMenu::moveSelection(int delta) {
    const auto items = visibleItems();
    if(items.isEmpty()) {
        m_selectedIndex = -1;
        return;
    }
    m_selectedIndex = m_selectedIndex < 0 ? (delta > 0 ? 0 : items.size() - 1)
                                          : (m_selectedIndex + delta + items.size()) % items.size();
    applySelection();
}

void ContextMenu::activateSelected() {
    const auto items = visibleItems();
    if(m_selectedIndex < 0 || m_selectedIndex >= items.size())
        return;
    hide();
    items[m_selectedIndex]->activate();
}

void ContextMenu::fillOpenWithMenu() {
    auto scripts = scriptManager->allScripts();
    QMap<QString, Script>::iterator i;
    for(i = scripts.begin(); i != scripts.end(); ++i) {
        if(!i.value().command.isEmpty()) {
            auto btn = new ContextMenuItem();
            btn->setAction("s:" + i.key());
            btn->setIconPath(":/res/icons/common/menuitem/open16.png");
            btn->setText(i.key());
            m_ui->scriptsLayout->addWidget(btn);
        }
    }
}

void ContextMenu::switchToMainPage() {
    m_ui->stackedWidget->setCurrentIndex(0);
}

void ContextMenu::switchToScriptsPage() {
    m_ui->stackedWidget->setCurrentIndex(1);
}

void ContextMenu::setImageEntriesEnabled(bool mode) {
    m_ui->rotateLeft->setEnabled(mode);
    m_ui->rotateRight->setEnabled(mode);
    m_ui->flipH->setEnabled(mode);
    m_ui->flipV->setEnabled(mode);
    m_ui->crop->setEnabled(mode);
    m_ui->resize->setEnabled(mode);

    m_ui->copy->setEnabled(mode);
    m_ui->move->setEnabled(mode);
    m_ui->trash->setEnabled(mode);
    m_ui->openWith->setEnabled(mode);
    m_ui->showLocation->setEnabled(mode);
}

void ContextMenu::showAt(QPoint pos) {
    switchToMainPage();
    clearSelection();
    QRect geom = geometry();
    geom.moveTopLeft(pos);
    setGeometry(geom);
    show();
}

void ContextMenu::setGeometry(QRect geom) {
    auto screen = QGuiApplication::screenAt(cursor().pos());
    if(!screen) // fallback
        screen = QGuiApplication::primaryScreen();
    if(screen) {
        // fit inside the current screen
        if(geom.bottom() > screen->geometry().bottom())
            geom.moveBottom(cursor().pos().y());
        if(geom.right() > screen->geometry().right())
            geom.moveRight(screen->geometry().right());
    }
    QWidget::setGeometry(geom);
}

void ContextMenu::mousePressEvent(QMouseEvent *event) {
    QWidget::mousePressEvent(event);
    hide();
}

void ContextMenu::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

// A popup owns its keyboard: while this menu is up, no application shortcut
// may fire behind it ("F used to toggle fullscreen with the menu up" — the
// old keyPressEvent comment). Qt delivers ShortcutOverride to the focused
// widget before the map fires anything; claiming every override keeps the
// navigation keys and Esc local, exactly what the swallow-everything handler
// below always did.
bool ContextMenu::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride)
        event->accept();
    return QWidget::event(event);
}

void ContextMenu::keyPressEvent(QKeyEvent *event) {
    // the popup owns the keyboard while open: navigation over its items,
    // Escape closes, and everything else is swallowed — an open menu must
    // not fire global actions behind itself (F used to toggle fullscreen
    // with the menu up, and Esc double-fired closeFullScreenOrExit)
    switch(event->key()) {
    case Qt::Key_Escape: hide(); return;
    case Qt::Key_Up: moveSelection(-1); return;
    case Qt::Key_Down: moveSelection(1); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: activateSelected(); return;
    default: break;
    }
}
