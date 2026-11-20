#include "settingsidebar.h"

#include <utility>

SettingsSidebar::SettingsSidebar(QWidget *parent) : QWidget{parent} {
    m_layout = new QBoxLayout(QBoxLayout::TopToBottom);
    m_layout->setSpacing(0);
    m_layout->setContentsMargins(8, 8, 9, 9);
    m_layout->addStretch();
    setLayout(m_layout);
    addEntry(":res/icons/common/settings/general32.png", tr("General"));
    addEntry(":res/icons/common/settings/view32.png", tr("View"));
    addEntry(":res/icons/common/settings/appearance32.png", tr("Theme"));
    addEntry(":res/icons/common/settings/shortcuts32.png", tr("Controls"));
    addEntry(":res/icons/common/settings/terminal32.png", tr("Scripts"));
    addEntry(":res/icons/common/settings/advanced32.png", tr("Advanced"));
    addEntry(":res/icons/common/settings/about32.png", tr("About"));
}

void SettingsSidebar::addEntry(QString icon, QString name) {
    SettingsSidebarItem *entry = new SettingsSidebarItem(std::move(icon), std::move(name));
    m_layout->insertWidget(m_entries.count(), entry);
    m_entries.append(entry);
    if(m_entries.count() == 1)
        selectEntry(0);
}

void SettingsSidebar::selectEntry(int idx) {
    if(idx >= 0 && idx < m_entries.count()) {
        foreach(auto entry, m_entries)
            entry->setHighlighted(false);
        m_entries[idx]->setHighlighted(true);
        emit entrySelected(idx);
    }
}

void SettingsSidebar::mousePressEvent(QMouseEvent *event) {
    event->accept();
    if(!(event->buttons() & Qt::LeftButton))
        return;
    selectEntryAt(event->pos());
}

void SettingsSidebar::mouseMoveEvent(QMouseEvent *event) {
    event->accept();
    if(!(event->buttons() & Qt::LeftButton))
        return;
    selectEntryAt(event->pos());
}

void SettingsSidebar::selectEntryAt(QPoint pos) {
    int newSelection = -1;
    for(int i = 0; i < m_entries.count(); i++) {
        if(m_entries[i]->geometry().contains(pos)) {
            if(!m_entries[i]->highlighted())
                newSelection = i;
            break;
        }
    }
    selectEntry(newSelection);
}

void SettingsSidebar::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

// -------------------------------------------------------------------

SettingsSidebarItem::SettingsSidebarItem(const QString &icon, const QString &name, QWidget *parent) : QWidget{parent} {
    QPalette p;
    if(p.base().color().valueF() <= 0.45f)
        m_iconWidget.setColor(QColor(184, 184, 185));
    else
        m_iconWidget.setColor(QColor(70, 70, 70));
    m_iconWidget.setIconPath(icon);
    m_textLabel.setText(name);
    m_layout = new QBoxLayout(QBoxLayout::LeftToRight);
    m_layout->setContentsMargins(6, 4, 6, 4);
    m_layout->setSpacing(7);
    m_layout->addWidget(&m_iconWidget);
    m_layout->addWidget(&m_textLabel);
    m_layout->addStretch();
    setLayout(m_layout);
    setMouseTracking(true);
}

void SettingsSidebarItem::setHighlighted(bool mode) {
    m_highlighted = mode;
    this->setProperty("checked", m_highlighted);
    style()->unpolish(this);
    style()->polish(this);
}

bool SettingsSidebarItem::highlighted() {
    return m_highlighted;
}

void SettingsSidebarItem::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
