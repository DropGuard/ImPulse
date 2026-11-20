#include "menuitem.h"

MenuItem::MenuItem(QWidget *parent) : QWidget(parent) {
    m_layout.setContentsMargins(6, 0, 8, 0);
    m_layout.setSpacing(2);

    setAccessibleName("MenuItem");
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_textLabel.setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_shortcutLabel.setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_iconWidget.setMinimumSize(26, 26); // 5px padding from stylesheet

    m_iconWidget.installEventFilter(this);

    // the spacer's ownership goes to m_layout the moment it is added, so it is a
    // local: a member here is a future delete-double-free waiting to be written
    auto spacer = new QSpacerItem(3, 1, QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_iconWidget.setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_iconWidget.setAccessibleName("MenuItemIcon");
    m_textLabel.setAccessibleName("MenuItemText");
    m_shortcutLabel.setAccessibleName("MenuItemShortcutLabel");
    m_layout.addWidget(&m_iconWidget);
    m_layout.addWidget(&m_textLabel);
    m_layout.addSpacerItem(spacer);
    m_layout.addWidget(&m_shortcutLabel);
    m_layout.setStretch(1, 1);

    setLayout(&m_layout);
}

void MenuItem::setText(const QString &text) {
    this->m_textLabel.setText(text);
}

QString MenuItem::text() {
    return m_textLabel.text();
}

void MenuItem::setShortcutText(const QString &text) {
    this->m_shortcutLabel.setText(text);
    this->adjustSize();
}

QString MenuItem::shortcut() {
    return m_shortcutLabel.text();
}

void MenuItem::setIconPath(const QString &path) {
    m_iconWidget.setIconPath(path);
}

void MenuItem::setPassthroughClicks(bool mode) {
    m_passthroughClicks = mode;
}

void MenuItem::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void MenuItem::onPress() {
}

void MenuItem::onClick() {
}

void MenuItem::mousePressEvent(QMouseEvent *event) {
    onPress();
    QWidget::mousePressEvent(event);
    if(!m_passthroughClicks)
        event->accept();
}

void MenuItem::mouseReleaseEvent(QMouseEvent *event) {
    onClick();
    QWidget::mouseReleaseEvent(event);
    if(!m_passthroughClicks)
        event->accept();
}
