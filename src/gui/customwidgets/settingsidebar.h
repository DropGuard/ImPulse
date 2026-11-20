#ifndef SETTINGSIDEBAR_H
#define SETTINGSIDEBAR_H

#include <QWidget>
#include <QLabel>
#include <QPainter>
#include <QStyleOption>
#include <QBoxLayout>
#include "gui/customwidgets/iconwidget.h"

class SettingsSidebarItem;

class SettingsSidebar : public QWidget {
    Q_OBJECT
  public:
    explicit SettingsSidebar(QWidget *parent = nullptr);
    void addEntry(QString icon, QString name);
    void selectEntry(int idx);

  private:
    QBoxLayout *m_layout;
    QList<SettingsSidebarItem *> m_entries;
    Qt::Orientation m_orientation = Qt::Vertical;
    void selectEntryAt(QPoint pos);

  signals:
    void entrySelected(int);

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    void paintEvent(QPaintEvent *event) override;
};

class SettingsSidebarItem : public QWidget {
    Q_OBJECT
  public:
    explicit SettingsSidebarItem(const QString &icon, const QString &name, QWidget *parent = nullptr);
    void setHighlighted(bool mode);
    bool highlighted();

  private:
    QBoxLayout *m_layout;
    IconWidget m_iconWidget;
    QLabel m_textLabel;
    bool m_highlighted = false;

  protected:
    void paintEvent(QPaintEvent *event) override;
};

#endif // SETTINGSIDEBAR_H
