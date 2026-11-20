#include "spinboxinputfix.h"

#include "components/actionmanager/actionmanager.h"

#include <utility>

SpinBoxInputFix::SpinBoxInputFix(QWidget *parent) : QSpinBox(parent) {
    // one source of truth: each entry is {logical key, scancode-table name}.
    // The two gates need different vocabularies — ShortcutOverride carries the
    // logical key, keyPressEvent resolves the scancode to a token name — so
    // both allowed sets are derived from this one table. "Enter" is the token
    // both Returns resolve to; PgUp/PgDown/Del are the table's spellings of
    // PageUp/PageDown/Delete.
    const std::initializer_list<std::pair<Qt::Key, const char *>> ownedKeys{
        {Qt::Key_0, "1"},         {Qt::Key_1, "2"},         {Qt::Key_2, "3"},         {Qt::Key_3, "4"},
        {Qt::Key_4, "5"},         {Qt::Key_5, "6"},         {Qt::Key_6, "7"},         {Qt::Key_7, "8"},
        {Qt::Key_8, "9"},         {Qt::Key_9, "0"},         {Qt::Key_Up, "Up"},       {Qt::Key_Down, "Down"},
        {Qt::Key_Left, "Left"},   {Qt::Key_Right, "Right"}, {Qt::Key_PageUp, "PgUp"}, {Qt::Key_PageDown, "PgDown"},
        {Qt::Key_Home, "Home"},   {Qt::Key_End, "End"},     {Qt::Key_Delete, "Del"},  {Qt::Key_Backspace, "Backspace"},
        {Qt::Key_Enter, "Enter"}, {Qt::Key_Return, "Enter"}};
    for(const auto &[key, name] : ownedKeys) {
        m_allowedLogicalKeys.insert(key);
        m_allowedKeys << QString::fromLatin1(name);
    }
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
           m_allowedLogicalKeys.contains(static_cast<Qt::Key>(ke->key())))
            event->accept();
    }
    return QSpinBox::event(event);
}

void SpinBoxInputFix::keyPressEvent(QKeyEvent *event) {
    quint32 nativeScanCode = event->nativeScanCode();
    QString key = actionManager->keyForNativeScancode(nativeScanCode);
    if(m_allowedKeys.contains(key)) {
        QSpinBox::keyPressEvent(event);
    } else {
        event->ignore();
    }
}
