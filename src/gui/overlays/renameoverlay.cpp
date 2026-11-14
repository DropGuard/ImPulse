#include "renameoverlay.h"
#include "ui_renameoverlay.h"

#include "shortcutbuilder.h"

RenameOverlay::RenameOverlay(FloatingWidgetContainer *parent) : OverlayWidget(parent), ui(new Ui::RenameOverlay) {
    ui->setupUi(this);
    // the keyboard veto rides the focused editor — see eventFilter below
    ui->fileName->installEventFilter(this);
    connect(ui->cancelButton, &QPushButton::clicked, this, &RenameOverlay::onCancel);
    connect(ui->closeButton, &IconButton::clicked, this, &RenameOverlay::hide);
    connect(ui->okButton, &QPushButton::clicked, this, &RenameOverlay::rename);
    ui->okButton->setHighlighted(true);
    ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    ui->headerIcon->setIconPath(":res/icons/common/overlay/edit16.png");
    setPosition(FloatingWidgetPosition::CENTER);
    setAcceptKeyboardFocus(true);

    keyFilter.append(actionManager->shortcutsForAction("exit"));
    keyFilter.append(actionManager->shortcutsForAction("renameFile"));
    // the veto filter reads sequences: ShortcutOverride events carry the
    // logical key, not the scancode the token table answers to
    for(const QString &token : std::as_const(keyFilter)) {
        keyFilterSequences.append(QKeySequence::fromString(token));
        if(token == QStringLiteral("Enter"))
            keyFilterSequences.append(QKeySequence(QStringLiteral("Return")));
    }

    hide();
    if(parent)
        setContainerSize(parent->size());
}

RenameOverlay::~RenameOverlay() {
    delete ui;
}

void RenameOverlay::show() {
    selectName();
    OverlayWidget::show();
    QTimer::singleShot(0, ui->fileName, SLOT(setFocus()));
}

void RenameOverlay::hide() {
    OverlayWidget::hide();
}

void RenameOverlay::setName(const QString &name) {
    ui->fileName->setText(name);
    origName = name;
    selectName();
}

void RenameOverlay::setBackdropEnabled(bool mode) {
    if(backdrop == mode)
        return;
    backdrop = mode;
    recalculateGeometry();
}

void RenameOverlay::recalculateGeometry() {
    if(!backdrop)
        OverlayWidget::recalculateGeometry();
    else // expand
        setGeometry(0, 0, containerSize().width(), containerSize().height());
}

void RenameOverlay::selectName() {
    int end = ui->fileName->text().lastIndexOf(".");
    if(end < 0)
        end = ui->fileName->text().length();
    ui->fileName->setSelection(0, end);
}

void RenameOverlay::rename() {
    if(ui->fileName->text().isEmpty())
        return;
    hide();
    emit renameRequested(ui->fileName->text());
}

void RenameOverlay::onCancel() {
    hide();
    ui->fileName->setText(origName);
}

// The line edit holds focus while renaming, and ShortcutOverride is delivered
// to the focused widget — so the veto lives in a filter on the edit. Keys in
// the curated passthrough (the exit/renameFile chords) stay with QShortcutMap
// and fire exactly where the old ignore()-and-propagate path sent them;
// everything else the overlay would swallow is claimed back: Esc, Enter and
// the navigation keys would otherwise fire closeFullScreenOrExit, folderView
// or the scroll actions in the middle of typing a filename.
bool RenameOverlay::eventFilter(QObject *watched, QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if(!keyFilterSequences.contains(QKeySequence(ke->key() | ke->modifiers())))
            event->accept();
    }
    return OverlayWidget::eventFilter(watched, event);
}

void RenameOverlay::keyPressEvent(QKeyEvent *event) {
    if(event->key() == Qt::Key_Escape) {
        event->accept();
        onCancel();
    } else if(event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        event->accept();
        rename();
    } else {
        auto shortcut = ShortcutBuilder::fromEvent(event);
        if(!shortcut.isEmpty() && keyFilter.contains(shortcut))
            event->ignore();
        else
            event->accept();
    }
}

void RenameOverlay::mousePressEvent(QMouseEvent *event) {
    event->accept();
    if(qApp->widgetAt(mapToGlobal(event->pos())) == this)
        this->onCancel();
}
