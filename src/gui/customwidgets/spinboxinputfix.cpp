#include "spinboxinputfix.h"

#include "components/actionmanager/actionmanager.h"

SpinBoxInputFix::SpinBoxInputFix(QWidget *parent)
    : QSpinBox(parent),
      allowedLogicalKeys{Qt::Key_0,        Qt::Key_1,     Qt::Key_2,    Qt::Key_3,      Qt::Key_4,
                         Qt::Key_5,        Qt::Key_6,     Qt::Key_7,    Qt::Key_8,      Qt::Key_9,
                         Qt::Key_Up,       Qt::Key_Down,  Qt::Key_Left, Qt::Key_Right,  Qt::Key_PageUp,
                         Qt::Key_PageDown, Qt::Key_Home,  Qt::Key_End,  Qt::Key_Delete, Qt::Key_Backspace,
                         Qt::Key_Enter,    Qt::Key_Return} {
    allowedKeys << "1"
                << "2"
                << "3"
                << "4"
                << "5"
                << "6"
                << "7"
                << "8"
                << "9"
                << "0"
                << "Up"
                << "Down"
                << "Left"
                << "Right"
                << "PgUp"
                << "PgDown"
                << "Enter"
                << "Home"
                << "End"
                << "Del"
                << "Backspace";
}

// The spin boxes inside the crop panel must own their editing keys: Qt
// vetoes only the printable keys for us (dispatch_probe_tests pins that
// arrows and Esc fire application shortcuts right through a line edit), so
// every key this class lets through to QSpinBox is claimed here. Keys outside
// allowedKeys stay unclaimed and fire their global actions through the map —
// the same place the ignored() propagation used to deliver them.
bool SpinBoxInputFix::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride) {
        // ShortcutOverride events carry the logical key only — the scancode
        // table cannot answer for them, so the claim is by key value. The
        // keypad modifier rides along: numpad digits and Enter are the same
        // keys to a spin box.
        auto *ke = static_cast<QKeyEvent *>(event);
        if((ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier &&
           allowedLogicalKeys.contains(static_cast<Qt::Key>(ke->key())))
            event->accept();
    }
    return QSpinBox::event(event);
}

void SpinBoxInputFix::keyPressEvent(QKeyEvent *event) {
    quint32 nativeScanCode = event->nativeScanCode();
    QString key = actionManager->keyForNativeScancode(nativeScanCode);
    if(allowedKeys.contains(key)) {
        QSpinBox::keyPressEvent(event);
    } else {
        event->ignore();
    }
}
