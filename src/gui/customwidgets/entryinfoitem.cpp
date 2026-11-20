#include "entryinfoitem.h"

#include <utility>

EntryInfoItem::EntryInfoItem(QWidget *parent) : QWidget(parent) {
    m_layout.setContentsMargins(9, 0, 9, 0);
    m_layout.setSpacing(0);
    m_layout.addWidget(&m_nameLabel);
    m_layout.addWidget(&m_valueLabel);
    setLayout(&m_layout);

    m_nameLabel.setFixedSize(120, 30);
    m_valueLabel.setFixedSize(142, 30);

    // add some padding for easier text selection
    m_valueLabel.setContentsMargins(3, 0, 0, 0);
    m_valueLabel.setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_valueLabel.setCursor(Qt::IBeamCursor);
}

void EntryInfoItem::setInfo(QString _name, QString _value) {
    m_name = std::move(_name);
    m_value = std::move(_value);
    m_nameLabel.setText(m_name);
    m_valueLabel.setText(m_value);
};

void EntryInfoItem::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
