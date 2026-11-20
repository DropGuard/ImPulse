#include "sidepanel.h"
#include "ui_sidepanel.h"

SidePanel::SidePanel(QWidget *parent) : QWidget(parent), m_ui(new Ui::SidePanel), m_widget(nullptr) {
    m_ui->setupUi(this);
    this->setObjectName("SidePanel");
    this->hide();
}

SidePanel::~SidePanel() {
    delete m_ui;
}

void SidePanel::setWidget(SidePanelWidget *w) {
    if(m_widget) {
        m_widget->hide();
        m_ui->layout->removeWidget(m_widget);
    }
    m_widget = w;
    m_ui->layout->addWidget(w);
    w->show();
}

SidePanelWidget *SidePanel::widget() {
    return m_widget;
}

void SidePanel::show() {
    QWidget::show();
    if(m_widget)
        m_widget->show();
}

void SidePanel::hide() {
    if(m_widget) {
        m_widget->hide();
    }
    QWidget::hide();
}

void SidePanel::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
