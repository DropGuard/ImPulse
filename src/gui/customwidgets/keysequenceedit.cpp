#include "keysequenceedit.h"

KeySequenceEdit::KeySequenceEdit(QWidget *parent) : QPushButton(parent), m_sequence("") {
    setFocusPolicy(Qt::StrongFocus); // the capture device must be focusable on every style
    QPushButton::setText(tr("[Enter sequence]"));
}

const QString &KeySequenceEdit::sequence() {
    return m_sequence;
}

void KeySequenceEdit::setText(const QString &text) {
    m_sequence = text;
    QPushButton::setText(m_sequence);
}

// The capture device: it must see every raw key, including the ones the
// global table binds (F, Ctrl+S, ...). Claiming every ShortcutOverride
// delivers them here instead of to QShortcutMap. StrongFocus, because the
// style-derived default for a QPushButton can be NoFocus.
bool KeySequenceEdit::event(QEvent *e) {
    if(e->type() == QEvent::ShortcutOverride)
        e->accept();
    return QPushButton::event(e);
}

void KeySequenceEdit::keyPressEvent(QKeyEvent *e) {
    processEvent(e);
}

void KeySequenceEdit::mousePressEvent(QMouseEvent *e) {
    processEvent(e);
}

void KeySequenceEdit::mouseReleaseEvent(QMouseEvent *e) {
    processEvent(e);
}

void KeySequenceEdit::wheelEvent(QWheelEvent *e) {
    processEvent(e);
}

// block native tab-switching so we can use it in shortcuts
bool KeySequenceEdit::focusNextPrevChild(bool) {
    return false;
}

void KeySequenceEdit::processEvent(QEvent *e) {
    QInputEvent *inputEvent = dynamic_cast<QInputEvent *>(e);
    if(!inputEvent)
        return;
    QString tmp = ShortcutBuilder::fromEvent(inputEvent);
    if(!tmp.isEmpty()) {
        m_sequence = tmp;
        this->setText(m_sequence);
        emit edited();
    }
}
