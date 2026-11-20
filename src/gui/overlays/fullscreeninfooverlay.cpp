#include "fullscreeninfooverlay.h"
#include "ui_fullscreeninfooverlay.h"

FullscreenInfoOverlay::FullscreenInfoOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), m_ui(new Ui::FullscreenInfoOverlay) {
    m_ui->setupUi(this);
    setPosition(FloatingWidgetPosition::TOPLEFT);
    this->setHorizontalMargin(0);
    this->setVerticalMargin(0);
    m_ui->nameLabel->setText("No file opened");
    if(parent)
        setContainerSize(parent->size());
}

FullscreenInfoOverlay::~FullscreenInfoOverlay() {
    delete m_ui;
}

void FullscreenInfoOverlay::setInfo(const QString &pos, const QString &fileName, const QString &info) {
    m_ui->posLabel->setText(pos);
    m_ui->nameLabel->setText(fileName);
    m_ui->infoLabel->setText(info);
    this->adjustSize();
}
