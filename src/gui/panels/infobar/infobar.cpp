#include "infobar.h"
#include "ui_infobar.h"

InfoBar::InfoBar(QWidget *parent) : QWidget(parent), m_ui(new Ui::InfoBar) {
    m_ui->setupUi(this);
    m_ui->path->setText("No file opened.");
}

InfoBar::~InfoBar() {
    delete m_ui;
}

void InfoBar::setInfo(const QString &position, const QString &fileName, const QString &info) {
    m_ui->index->setText(position);
    m_ui->path->setText(fileName);
    m_ui->info->setText(info);
}

void InfoBar::wheelEvent(QWheelEvent *event) {
    event->accept();
}

void InfoBar::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
