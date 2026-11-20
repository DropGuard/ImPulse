#include "inputmap.h"

InputMap *inputMap = nullptr;

InputMap::InputMap() {
    initKeyMap();
    initModMap();
}

InputMap *InputMap::getInstance() {
    if(!inputMap) {
        inputMap = new InputMap();
    }
    return inputMap;
}

const QMap<quint32, QString> &InputMap::keys() {
    return m_keyMap;
}

const QMap<QString, Qt::KeyboardModifier> &InputMap::modifiers() {
    return m_modMap;
}

void InputMap::initKeyMap() {
    // key codes as reported by QKeyEvent::nativeScanCode()
    m_keyMap.clear();
#ifdef _WIN32
    // windows keymap for qimgv

    // row 1
    keyMap.insert(1, "Esc");
    keyMap.insert(59, "F1");
    keyMap.insert(60, "F2");
    keyMap.insert(61, "F3");
    keyMap.insert(62, "F4");
    keyMap.insert(63, "F5");
    keyMap.insert(64, "F6");
    keyMap.insert(65, "F7");
    keyMap.insert(66, "F8");
    keyMap.insert(67, "F9");
    keyMap.insert(68, "F10");
    keyMap.insert(87, "F11");
    keyMap.insert(88, "F12");
    //keyMap.insert(  , "Print" );
    keyMap.insert(70, "ScrollLock");
    keyMap.insert(69, "Pause");

    // row 2
    keyMap.insert(41, "`");
    keyMap.insert(2, "1");
    keyMap.insert(3, "2");
    keyMap.insert(4, "3");
    keyMap.insert(5, "4");
    keyMap.insert(6, "5");
    keyMap.insert(7, "6");
    keyMap.insert(8, "7");
    keyMap.insert(9, "8");
    keyMap.insert(10, "9");
    keyMap.insert(11, "0");
    keyMap.insert(12, "-");
    keyMap.insert(13, "=");
    keyMap.insert(14, "Backspace");
    keyMap.insert(338, "Ins");
    keyMap.insert(327, "Home");
    keyMap.insert(329, "PgUp");

    // row 3
    keyMap.insert(15, "Tab");
    keyMap.insert(16, "Q");
    keyMap.insert(17, "W");
    keyMap.insert(18, "E");
    keyMap.insert(19, "R");
    keyMap.insert(20, "T");
    keyMap.insert(21, "Y");
    keyMap.insert(22, "U");
    keyMap.insert(23, "I");
    keyMap.insert(24, "O");
    keyMap.insert(25, "P");
    keyMap.insert(26, "[");
    keyMap.insert(27, "]");
    keyMap.insert(28, "Enter"); // Qt outputs "Return" here and "Enter" on numpad
    keyMap.insert(339, "Del");
    keyMap.insert(335, "End");
    keyMap.insert(337, "PgDown");

    // row 4
    keyMap.insert(58, "CapsLock");
    keyMap.insert(30, "A");
    keyMap.insert(31, "S");
    keyMap.insert(32, "D");
    keyMap.insert(33, "F");
    keyMap.insert(34, "G");
    keyMap.insert(35, "H");
    keyMap.insert(36, "J");
    keyMap.insert(37, "K");
    keyMap.insert(38, "L");
    keyMap.insert(39, ";");
    keyMap.insert(40, "'");
    keyMap.insert(43, "\\");

    // row 5
    keyMap.insert(44, "Z");
    keyMap.insert(45, "X");
    keyMap.insert(46, "C");
    keyMap.insert(47, "V");
    keyMap.insert(48, "B");
    keyMap.insert(49, "N");
    keyMap.insert(50, "M");
    keyMap.insert(51, ",");
    keyMap.insert(52, ".");
    keyMap.insert(53, "/");
    keyMap.insert(328, "Up");

    // row 6
    keyMap.insert(57, "Space");
    keyMap.insert(349, "Menu");
    keyMap.insert(331, "Left");
    keyMap.insert(336, "Down");
    keyMap.insert(333, "Right");

    // numpad
    keyMap.insert(325, "NumLock");
    keyMap.insert(309, "/");
    keyMap.insert(55, "*");
    keyMap.insert(74, "-");
    keyMap.insert(71, "7");
    keyMap.insert(72, "8");
    keyMap.insert(73, "9");
    keyMap.insert(78, "+");
    keyMap.insert(75, "4");
    keyMap.insert(76, "5");
    keyMap.insert(77, "6");
    keyMap.insert(79, "1");
    keyMap.insert(80, "2");
    keyMap.insert(81, "3");
    keyMap.insert(284, "Enter");
    keyMap.insert(82, "0");
    keyMap.insert(83, ".");

    // special
    //keyMap.insert( ?? , "Wake Up" ); // "Fn" key on thinkpad
    keyMap.insert(86, "<"); // near left shift (iso layout)
    //keyMap.insert(??, "PgBack");
    //keyMap.insert(??, "PgForward");

    // looks like qt 6.7.0 changed nativeScanCode() values on windows
    // see https://github.com/easymodo/qimgv/issues/539
    keyMap.insert(57426, "Ins");
    keyMap.insert(57415, "Home");
    keyMap.insert(57417, "PgUp");
    keyMap.insert(57427, "Del");
    keyMap.insert(57423, "End");
    keyMap.insert(57425, "PgDown");
    keyMap.insert(57416, "Up");
    keyMap.insert(57437, "Menu");
    keyMap.insert(57419, "Left");
    keyMap.insert(57424, "Down");
    keyMap.insert(57421, "Right");
    // numpad
    keyMap.insert(57413, "NumLock");
    keyMap.insert(57397, "/");
    keyMap.insert(57372, "Enter");

#elif defined(__linux__) || defined(__FreeBSD__)
    // linux keymap for qimgv

    // row 1
    m_keyMap.insert(9, "Esc");
    m_keyMap.insert(67, "F1");
    m_keyMap.insert(68, "F2");
    m_keyMap.insert(69, "F3");
    m_keyMap.insert(70, "F4");
    m_keyMap.insert(71, "F5");
    m_keyMap.insert(72, "F6");
    m_keyMap.insert(73, "F7");
    m_keyMap.insert(74, "F8");
    m_keyMap.insert(75, "F9");
    m_keyMap.insert(76, "F10");
    m_keyMap.insert(95, "F11");
    m_keyMap.insert(96, "F12");
    m_keyMap.insert(107, "Print");
    m_keyMap.insert(78, "ScrollLock");
    m_keyMap.insert(127, "Pause");

    // row 2
    m_keyMap.insert(49, "`");
    m_keyMap.insert(10, "1");
    m_keyMap.insert(11, "2");
    m_keyMap.insert(12, "3");
    m_keyMap.insert(13, "4");
    m_keyMap.insert(14, "5");
    m_keyMap.insert(15, "6");
    m_keyMap.insert(16, "7");
    m_keyMap.insert(17, "8");
    m_keyMap.insert(18, "9");
    m_keyMap.insert(19, "0");
    m_keyMap.insert(20, "-");
    m_keyMap.insert(21, "=");
    m_keyMap.insert(22, "Backspace");
    m_keyMap.insert(118, "Ins");
    m_keyMap.insert(110, "Home");
    m_keyMap.insert(112, "PgUp");

    // row 3
    m_keyMap.insert(23, "Tab");
    m_keyMap.insert(24, "Q");
    m_keyMap.insert(25, "W");
    m_keyMap.insert(26, "E");
    m_keyMap.insert(27, "R");
    m_keyMap.insert(28, "T");
    m_keyMap.insert(29, "Y");
    m_keyMap.insert(30, "U");
    m_keyMap.insert(31, "I");
    m_keyMap.insert(32, "O");
    m_keyMap.insert(33, "P");
    m_keyMap.insert(34, "[");
    m_keyMap.insert(35, "]");
    m_keyMap.insert(36, "Enter"); // Qt outputs "Return" here and "Enter" on numpad
    m_keyMap.insert(119, "Del");
    m_keyMap.insert(115, "End");
    m_keyMap.insert(117, "PgDown");

    // row 4
    m_keyMap.insert(66, "CapsLock");
    m_keyMap.insert(38, "A");
    m_keyMap.insert(39, "S");
    m_keyMap.insert(40, "D");
    m_keyMap.insert(41, "F");
    m_keyMap.insert(42, "G");
    m_keyMap.insert(43, "H");
    m_keyMap.insert(44, "J");
    m_keyMap.insert(45, "K");
    m_keyMap.insert(46, "L");
    m_keyMap.insert(47, ";");
    m_keyMap.insert(48, "'");
    m_keyMap.insert(51, "\\");

    // row 5
    m_keyMap.insert(52, "Z");
    m_keyMap.insert(53, "X");
    m_keyMap.insert(54, "C");
    m_keyMap.insert(55, "V");
    m_keyMap.insert(56, "B");
    m_keyMap.insert(57, "N");
    m_keyMap.insert(58, "M");
    m_keyMap.insert(59, ",");
    m_keyMap.insert(60, ".");
    m_keyMap.insert(61, "/");
    m_keyMap.insert(111, "Up");

    // row 6
    m_keyMap.insert(65, "Space");
    m_keyMap.insert(135, "Menu");
    m_keyMap.insert(113, "Left");
    m_keyMap.insert(116, "Down");
    m_keyMap.insert(114, "Right");

    // numpad
    m_keyMap.insert(77, "NumLock");
    m_keyMap.insert(106, "/");
    m_keyMap.insert(63, "*");
    m_keyMap.insert(82, "-");
    m_keyMap.insert(79, "7");
    m_keyMap.insert(80, "8");
    m_keyMap.insert(81, "9");
    m_keyMap.insert(86, "+");
    m_keyMap.insert(83, "4");
    m_keyMap.insert(84, "5");
    m_keyMap.insert(85, "6");
    m_keyMap.insert(87, "1");
    m_keyMap.insert(88, "2");
    m_keyMap.insert(89, "3");
    m_keyMap.insert(104, "Enter");
    m_keyMap.insert(90, "0");
    m_keyMap.insert(91, ".");

    // special
    m_keyMap.insert(151, "WakeUp"); // "Fn" key on thinkpad
    m_keyMap.insert(94, "<");       // near left shift (iso layout)
    m_keyMap.insert(166, "Back");
    m_keyMap.insert(167, "Forward");
#endif
}

void InputMap::initModMap() {
    m_modMap.clear();
    m_modMap.insert(keyNameCtrl(), Qt::ControlModifier);
    m_modMap.insert(keyNameAlt(), Qt::AltModifier);
    m_modMap.insert(keyNameShift(), Qt::ShiftModifier);
}

QString InputMap::keyNameCtrl() {
#ifdef __APPLE__
    return "⌘";
#else
    return "Ctrl";
#endif
}

QString InputMap::keyNameAlt() {
#ifdef __APPLE__
    return "⌥";
#else
    return "Alt";
#endif
}

QString InputMap::keyNameShift() {
#ifdef __APPLE__
    return "⇧";
#else
    return "Shift";
#endif
}
