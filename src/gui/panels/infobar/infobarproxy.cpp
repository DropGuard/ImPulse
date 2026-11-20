#include "infobarproxy.h"

InfoBarProxy::InfoBarProxy(QWidget *parent) : QWidget(parent), m_infoBar(nullptr) {
    setAccessibleName("InfoBarProxy");
    this->setMinimumHeight(23);
    this->setMaximumHeight(23);
    m_layout.setContentsMargins(0, 0, 0, 0);
    setLayout(&m_layout);
}

InfoBarProxy::~InfoBarProxy() {
    // infoBar is this widget's own child: ~QObject reclaims it, and doing it
    // from here meant touching a handle the child list had already released
}

void InfoBarProxy::setInfo(const QString &position, const QString &fileName, const QString &info) {
    if(m_infoBar) {
        m_infoBar->setInfo(position, fileName, info);
    } else {
        m_stateBuf.position = position;
        m_stateBuf.fileName = fileName;
        m_stateBuf.info = info;
    }
}

void InfoBarProxy::init() {
    if(m_infoBar)
        return;
    m_infoBar = new InfoBar(this);
    setFocusProxy(m_infoBar);
    m_layout.addWidget(m_infoBar);
    setLayout(&m_layout);
    if(!m_stateBuf.fileName.isEmpty())
        m_infoBar->setInfo(m_stateBuf.position, m_stateBuf.fileName, m_stateBuf.info);
}

void InfoBarProxy::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
