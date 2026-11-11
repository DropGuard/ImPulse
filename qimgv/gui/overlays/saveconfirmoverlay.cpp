#include "saveconfirmoverlay.h"
#include "ui_saveconfirmoverlay.h"
#include "components/actionmanager/actionmanager.h"

SaveConfirmOverlay::SaveConfirmOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), ui(new Ui::SaveConfirmOverlay) {
    ui->setupUi(this);
    connect(ui->saveButton, &QPushButton::clicked, this, &SaveConfirmOverlay::saveClicked);
    connect(ui->saveAsButton, &QPushButton::clicked, this, &SaveConfirmOverlay::saveAsClicked);
    connect(ui->discardButton, &QPushButton::clicked, this, &SaveConfirmOverlay::discardClicked);
    ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    ui->headerIcon->setIconPath(":res/icons/common/overlay/edit16.png");
    // the answers' keys on their faces — the platform standards for the two
    // writes (they live in the global table and work through this surface's
    // fall-through), Esc for the way out. The translated .ui text stays the
    // base, the hint is decoration.
    ui->saveButton->setText(ui->saveButton->text() + QStringLiteral("  [Ctrl+S]"));
    ui->saveAsButton->setText(ui->saveAsButton->text() + QStringLiteral("  [Ctrl+Shift+S]"));
    ui->discardButton->setText(ui->discardButton->text() + QStringLiteral("  [Esc]"));
    readSettings();
    connect(settings, &Settings::settingsChanged, this, &SaveConfirmOverlay::readSettings);

    if(parent)
        setContainerSize(parent->size());

    this->hide();
}

// the disk never sits on a bare key: Save and Save as are the platform
// chords — they live in the global table and reach this prompt's actions
// through its fall-through, exactly as the button hints say — and Esc drops
// the edits and dismisses (the disk file is untouched by a discard; only
// this session's edits are lost, which is what a single key may cost). A
// bare Enter is no answer at all: it must not overwrite in place, and it
// must not abandon the edits either — before it was captured it used to
// fall through to the folder-view toggle behind the prompt.
void SaveConfirmOverlay::keyPressEvent(QKeyEvent *event) {
    event->accept();
    if(event->key() == Qt::Key_Escape) {
        emit discardClicked();
        return;
    }
    if(event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        return;
    actionManager->processEvent(event);
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
