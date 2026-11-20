#pragma once

#include <QMap>
#include <QString>

class InputMap {
  public:
    InputMap();
    static InputMap *getInstance();
    const QMap<quint32, QString> &keys();
    const QMap<QString, Qt::KeyboardModifier> &modifiers();
    static QString keyNameCtrl();
    static QString keyNameAlt();
    static QString keyNameShift();

  private:
    void initKeyMap();
    void initModMap();
    QMap<quint32, QString> m_keyMap;
    QMap<QString, Qt::KeyboardModifier> m_modMap;
};

extern InputMap *inputMap;
