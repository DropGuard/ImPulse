#include "saveconfirmoverlay.h"
#include "ui_saveconfirmoverlay.h"
#include "components/actionmanager/actionmanager.h"

SaveConfirmOverlay::SaveConfirmOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), m_ui(new Ui::SaveConfirmOverlay) {
    m_ui->setupUi(this);
    connect(m_ui->saveButton, &QPushButton::clicked, this, &SaveConfirmOverlay::saveClicked);
    connect(m_ui->saveAsButton, &QPushButton::clicked, this, &SaveConfirmOverlay::saveAsClicked);
    connect(m_ui->discardButton, &QPushButton::clicked, this, &SaveConfirmOverlay::discardClicked);
    m_ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    m_ui->headerIcon->setIconPath(":res/icons/common/overlay/edit16.png");
    // the answers' keys on their faces — the platform standards for the two
    // writes (they live in the global table and work through this surface's
    // fall-through), Esc for the way out. The translated .ui text stays the
    // base, the hint is decoration.
    m_ui->saveButton->setText(m_ui->saveButton->text() + QStringLiteral("  [Ctrl+S]"));
    m_ui->saveAsButton->setText(m_ui->saveAsButton->text() + QStringLiteral("  [Ctrl+Shift+S]"));
    m_ui->discardButton->setText(m_ui->discardButton->text() + QStringLiteral("  [Esc]"));
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
// The keyboard dispatch is QShortcutMap's now: a projected key fires before
// this surface ever sees it. What still arrives is decided here — the walk,
// the view switch and the way out (keyNamesSubjectChangingAction) are vetoed back to
// local delivery, because a Page Down would leave "save or discard?" pointing
// at a file nobody is deciding about. The save chords are NOT vetoed: Ctrl+S
// reaches the save action straight through the map, exactly as the buttons'
// hint text promises.
bool SaveConfirmOverlay::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if(actionManager->keyNamesSubjectChangingAction(ke)) {
            event->accept();
            return true; // the claim dies if the base re-ignores the override
        }
    }
    return OverlayWidget::event(event);
}

void SaveConfirmOverlay::keyPressEvent(QKeyEvent *event) {
    event->accept();
    if(event->key() == Qt::Key_Escape) {
        emit discardClicked();
        return;
    }
    if(event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        return;
    // the buttons' chords (Ctrl+S / Ctrl+Shift+S) fire through QShortcutMap
    // before this handler runs; what arrives here is the walk the surface
    // vetoed back — consumed, because a new image would leave "save or
    // discard?" pointing at a file the user is no longer deciding about
    if(actionManager->keyNamesSubjectChangingAction(event))
        return;
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
    delete m_ui;
}
