#include "colorselectorbutton.h"

#include <utility>

ColorSelectorButton::ColorSelectorButton(QWidget *parent) : ClickableLabel(parent) {
    connect(this, &ColorSelectorButton::clicked, this, &ColorSelectorButton::showColorSelector);
}

void ColorSelectorButton::setColor(QColor &newColor) {
    m_color = newColor;
    update();
}

void ColorSelectorButton::setDescription(QString text) {
    this->m_description = std::move(text);
}

QColor ColorSelectorButton::color() {
    return m_color;
}

void ColorSelectorButton::showColorSelector() {
    QColor newColor = QColorDialog::getColor(m_color, this, m_description);
    if(newColor.isValid()) {
        m_color = newColor;
        update();
    }
}

void ColorSelectorButton::paintEvent(QPaintEvent *e) {
    Q_UNUSED(e)

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if(!this->isEnabled())
        p.setOpacity(0.5f);
    p.setPen(QColor(40, 40, 40));
    p.drawRect(QRectF(0.5f, 0.5f, width() - 1.0f, height() - 1.0f));
    p.fillRect(rect().adjusted(2, 2, -2, -2), m_color);
}
