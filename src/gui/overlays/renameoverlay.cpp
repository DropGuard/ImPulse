#include "renameoverlay.h"
#include "ui_renameoverlay.h"

#include "shortcutbuilder.h"

RenameOverlay::RenameOverlay(FloatingWidgetContainer *parent) : OverlayWidget(parent), m_ui(new Ui::RenameOverlay) {
    m_ui->setupUi(this);
    // the keyboard veto rides the focused editor — see eventFilter below
    m_ui->fileName->installEventFilter(this);
    connect(m_ui->cancelButton, &QPushButton::clicked, this, &RenameOverlay::onCancel);
    connect(m_ui->closeButton, &IconButton::clicked, this, &RenameOverlay::hide);
    connect(m_ui->okButton, &QPushButton::clicked, this, &RenameOverlay::rename);
    m_ui->okButton->setHighlighted(true);
    m_ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    m_ui->headerIcon->setIconPath(":res/icons/common/overlay/edit16.png");
    setPosition(FloatingWidgetPosition::CENTER);
    setAcceptKeyboardFocus(true);

    m_keyFilter.append(actionManager->shortcutsForAction("exit"));
    m_keyFilter.append(actionManager->shortcutsForAction("renameFile"));
    // the veto filter reads sequences: ShortcutOverride events carry the
    // logical key, not the scancode the token table answers to. The expansion
    // is ActionManager's own — the same one the projection uses.
    for(const QString &token : std::as_const(m_keyFilter))
        m_keyFilterSequences.append(ActionManager::sequencesForToken(token));

    hide();
    if(parent)
        setContainerSize(parent->size());
}

RenameOverlay::~RenameOverlay() {
    delete m_ui;
}

void RenameOverlay::show() {
    selectName();
    OverlayWidget::show();
    QTimer::singleShot(0, m_ui->fileName, SLOT(setFocus()));
}

void RenameOverlay::hide() {
    OverlayWidget::hide();
}

void RenameOverlay::setName(const QString &name) {
    m_ui->fileName->setText(name);
    m_origName = name;
    selectName();
}

void RenameOverlay::setBackdropEnabled(bool mode) {
    if(m_backdrop == mode)
        return;
    m_backdrop = mode;
    recalculateGeometry();
}

void RenameOverlay::recalculateGeometry() {
    if(!m_backdrop)
        OverlayWidget::recalculateGeometry();
    else // expand
        setGeometry(0, 0, containerSize().width(), containerSize().height());
}

void RenameOverlay::selectName() {
    int end = m_ui->fileName->text().lastIndexOf(".");
    if(end < 0)
        end = m_ui->fileName->text().length();
    m_ui->fileName->setSelection(0, end);
}

void RenameOverlay::rename() {
    if(m_ui->fileName->text().isEmpty())
        return;
    hide();
    emit renameRequested(m_ui->fileName->text());
}

void RenameOverlay::onCancel() {
    hide();
    m_ui->fileName->setText(m_origName);
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
        if(!m_keyFilterSequences.contains(QKeySequence(Qt::Key(ke->key()) | ke->modifiers())))
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
        if(!shortcut.isEmpty() && m_keyFilter.contains(shortcut))
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
