#pragma once

#include <QPushButton>
#include "shortcutbuilder.h"

class KeySequenceEdit : public QPushButton {
    Q_OBJECT
  public:
    KeySequenceEdit(QWidget *parent);
    const QString &sequence();
    void setText(const QString &text);

  protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

    void mouseReleaseEvent(QMouseEvent *e) override;
    bool focusNextPrevChild(bool) override;

  private:
    void processEvent(QEvent *e);
    QString m_sequence;

  signals:
    void edited();
};
