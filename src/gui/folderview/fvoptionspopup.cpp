#include "fvoptionspopup.h"
#include "ui_fvoptionspopup.h"

#include "components/actionmanager/actionmanager.h"

FVOptionsPopup::FVOptionsPopup(QWidget *parent) : QWidget(parent), m_ui(new Ui::FVOptionsPopup) {
    m_ui->setupUi(this);

    setWindowFlags(Qt::Popup);
    setAttribute(Qt::WA_TranslucentBackground, true);

    m_ui->viewSimpleButton->setText(QObject::tr("Simple"));
    m_ui->viewExtendedButton->setText(QObject::tr("Extended"));
    m_ui->viewFoldersButton->setText(QObject::tr("Extended + Folders"));

    connect(m_ui->viewSimpleButton, &ContextMenuItem::pressed, this, &FVOptionsPopup::selectSimpleView);
    connect(m_ui->viewExtendedButton, &ContextMenuItem::pressed, this, &FVOptionsPopup::selectExtendedView);
    connect(m_ui->viewFoldersButton, &ContextMenuItem::pressed, this, &FVOptionsPopup::selectFoldersView);

    // force size recalculation
    this->adjustSize();

    readSettings();
    connect(settings, &Settings::settingsChanged, this, &FVOptionsPopup::readSettings);

    hide();
}

FVOptionsPopup::~FVOptionsPopup() {
    delete m_ui;
}

void FVOptionsPopup::setSimpleView() {
    m_ui->viewSimpleButton->setIconPath(":res/icons/common/buttons/panel-small/add-new12.png");
    m_ui->viewExtendedButton->setIconPath("");
    m_ui->viewFoldersButton->setIconPath("");
}

void FVOptionsPopup::setExtendedView() {
    m_ui->viewSimpleButton->setIconPath("");
    m_ui->viewExtendedButton->setIconPath(":res/icons/common/buttons/panel-small/add-new12.png");
    m_ui->viewFoldersButton->setIconPath("");
}

void FVOptionsPopup::setFoldersView() {
    m_ui->viewSimpleButton->setIconPath("");
    m_ui->viewExtendedButton->setIconPath("");
    m_ui->viewFoldersButton->setIconPath(":res/icons/common/buttons/panel-small/add-new12.png");
}

void FVOptionsPopup::selectSimpleView() {
    setSimpleView();
    emit viewModeSelected(FV_SIMPLE);
}

void FVOptionsPopup::selectExtendedView() {
    setExtendedView();
    emit viewModeSelected(FV_EXTENDED);
}

void FVOptionsPopup::selectFoldersView() {
    setFoldersView();
    emit viewModeSelected(FV_EXT_FOLDERS);
}

void FVOptionsPopup::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

// Popup contract, same as ContextMenu: claim every ShortcutOverride so the
// application shortcuts cannot fire behind the popup.
bool FVOptionsPopup::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride)
        event->accept();
    return QWidget::event(event);
}

void FVOptionsPopup::keyPressEvent(QKeyEvent *event) {
    // an open popup must not fire global actions — keys used to forward to
    // the action system, so F behind this popup toggled fullscreen
    if(event->key() == Qt::Key_Escape)
        hide();
}

void FVOptionsPopup::setViewMode(FolderViewMode mode) {
    if(mode == FV_SIMPLE)
        setSimpleView();
    else if(mode == FV_EXTENDED)
        setExtendedView();
    else
        setFoldersView();
}

void FVOptionsPopup::readSettings() {
    setViewMode(settings->folderViewMode());
}

void FVOptionsPopup::showAt(QPoint pos) {
    QRect geom = geometry();
    geom.moveTopLeft(pos);
    setGeometry(geom);
    show();
}

void FVOptionsPopup::hideEvent(QHideEvent *event) {
    event->accept();
    emit dismissed();
}
