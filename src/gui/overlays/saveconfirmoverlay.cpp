#include "saveconfirmoverlay.h"
#include "ui_saveconfirmoverlay.h"

SaveConfirmOverlay::SaveConfirmOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), ui(new Ui::SaveConfirmOverlay) {
    ui->setupUi(this);
    connect(ui->saveButton, &QPushButton::clicked, this, &SaveConfirmOverlay::saveClicked);
    connect(ui->saveAsButton, &QPushButton::clicked, this, &SaveConfirmOverlay::saveAsClicked);
    connect(ui->discardButton, &QPushButton::clicked, this, &SaveConfirmOverlay::discardClicked);
    ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    ui->headerIcon->setIconPath(":res/icons/common/overlay/edit16.png");
    readSettings();
    connect(settings, &Settings::settingsChanged, this, &SaveConfirmOverlay::readSettings);

    if(parent)
        setContainerSize(parent->size());

    this->hide();
}

// the surface owns Esc, and Esc means the same thing as the discard button:
// drop the edits and dismiss. The chain runs through Core::discardEdits,
// which also hides this overlay.
void SaveConfirmOverlay::keyPressEvent(QKeyEvent *event) {
    if(event->key() == Qt::Key_Escape) {
        event->accept();
        emit discardClicked();
        return;
    }
    event->ignore();
}

void SaveConfirmOverlay::readSettings() {
    // don't interfere with the main panel
    if(settings->panelEnabled() && settings->panelPosition() == PanelPosition::PANEL_BOTTOM) {
        setPosition(FloatingWidgetPosition::TOPRIGHT);
    } else {
        setPosition(FloatingWidgetPosition::BOTTOMRIGHT);
    }
    update();
}

SaveConfirmOverlay::~SaveConfirmOverlay() {
    delete ui;
}
