// Base class for various menu items.
// Displays entry name, shortcut and an icon.

#pragma once

#include <QLabel>
#include <QStyleOption>
#include <QHBoxLayout>
#include <QSpacerItem>
#include "gui/customwidgets/iconbutton.h"
#include "components/actionmanager/actionmanager.h"

class MenuItem : public QWidget {
    Q_OBJECT
  public:
    MenuItem(QWidget *parent = nullptr);
    void setText(const QString &m_textLabel);
    QString text();
    void setShortcutText(const QString &m_textLabel);
    QString shortcut();
    void setIconPath(const QString &path);
    void setPassthroughClicks(bool mode);

  protected:
    IconButton m_iconWidget;
    QLabel m_textLabel, m_shortcutLabel;
    QHBoxLayout m_layout;
    bool m_passthroughClicks = true;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    virtual void onClick();
    virtual void onPress();
};
