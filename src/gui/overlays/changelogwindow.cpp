#include "changelogwindow.h"
#include "ui_changelogwindow.h"

#include "components/actionmanager/actionmanager.h"
#include "shortcutbuilder.h"

ChangelogWindow::ChangelogWindow(FloatingWidgetContainer *parent) : OverlayWidget(parent), ui(new Ui::ChangelogWindow) {
    ui->setupUi(this);
    hide();
    setPosition(FloatingWidgetPosition::CENTER);
    connect(ui->closeButton, &QPushButton::pressed, this, &ChangelogWindow::hide);
    connect(ui->shutUpButton, &QPushButton::pressed, this, &ChangelogWindow::hideAndShutUp);

    if(parent)
        setContainerSize(parent->size());
}

void ChangelogWindow::hideAndShutUp() {
    settings->setShowChangelogs(false);
    hide();
}

ChangelogWindow::~ChangelogWindow() {
    delete ui;
}

void ChangelogWindow::setText(const QString &text) {
    ui->textBrowser->setText(text);
}

void ChangelogWindow::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

// move to base class?
void ChangelogWindow::wheelEvent(QWheelEvent *event) {
    event->accept();
}

// The close button holds focus while this window is up, so the ShortcutOverride
// for Esc arrives there, not at the window — the filter claims it back. Without
// it, Esc would fire closeFullScreenOrExit behind a what's-new popup.
bool ChangelogWindow::event(QEvent *event) {
    return OverlayWidget::event(event);
}

bool ChangelogWindow::eventFilter(QObject *watched, QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if(QKeySequence(ke->key() | ke->modifiers()) == QKeySequence(QStringLiteral("Esc")))
            event->accept();
    }
    return OverlayWidget::eventFilter(watched, event);
}

void ChangelogWindow::keyPressEvent(QKeyEvent *event) {
    quint32 nativeScanCode = event->nativeScanCode();
    QString key = actionManager->keyForNativeScancode(nativeScanCode);
    if(key == "Esc") {
        event->accept();
        hide();
    }
}

void ChangelogWindow::show() {
    OverlayWidget::show();
    // the veto filter rides the buttons: they are what holds focus here
    ui->closeButton->installEventFilter(this);
    ui->shutUpButton->installEventFilter(this);
    ui->closeButton->setFocus();
}

void ChangelogWindow::hide() {
    ui->closeButton->clearFocus();
    ui->shutUpButton->clearFocus();
    OverlayWidget::hide();
}
