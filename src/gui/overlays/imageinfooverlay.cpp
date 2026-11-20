#include "imageinfooverlay.h"
#include "ui_imageinfooverlay.h"

ImageInfoOverlay::ImageInfoOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), m_ui(new Ui::ImageInfoOverlay) {
    m_ui->setupUi(this);
    m_ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    m_ui->headerIcon->setIconPath(":res/icons/common/overlay/info16.png");
    m_entryStub.setFixedSize(280, 48);
    m_entryStub.setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    connect(m_ui->closeButton, &IconButton::clicked, this, &ImageInfoOverlay::hide);
    this->setPosition(FloatingWidgetPosition::RIGHT);

    if(parent)
        setContainerSize(parent->size());
}

ImageInfoOverlay::~ImageInfoOverlay() {
    delete m_ui;
    for(auto i = m_entries.count() - 1; i >= 0; i--)
        delete m_entries.takeAt(i);
}

void ImageInfoOverlay::setExifInfo(QMap<QString, QString> info) {
    // remove/add entries
    int entryCount = m_entries.count();
    if(entryCount > info.count()) {
        for(auto i = entryCount - 1; i >= info.count(); i--) {
            m_ui->entryLayout->removeWidget(m_entries.last());
            delete m_entries.takeLast();
        }
    } else if(entryCount < info.count()) {
        for(auto i = entryCount; i < info.count(); i++) {
            m_entries.append(new EntryInfoItem(this));
            m_ui->entryLayout->addWidget(m_entries.last());
        }
    }
    QMap<QString, QString>::const_iterator i = info.constBegin();
    int entryIdx = 0;
    while(i != info.constEnd()) {
        m_entries.at(entryIdx)->setInfo(i.key(), i.value());
        ++i;
        ++entryIdx;
    }

    // Hiding/showing entryStub causes flicker,
    // so we just remove it from layout and clear the text.
    // It's still there but basically not visible
    if(m_entries.count()) {
        m_ui->entryLayout->removeWidget(&m_entryStub);
        m_entryStub.setText("");
    } else {
        m_ui->entryLayout->addWidget(&m_entryStub);
        m_entryStub.setText("<no metadata found>");
    }

    if(!isHidden() && entryCount != info.count()) {
        // wait for layout change
        qApp->processEvents();
        // reposition
        recalculateGeometry();
    }
}

void ImageInfoOverlay::show() {
    OverlayWidget::show();
    adjustSize();
    recalculateGeometry();
}

void ImageInfoOverlay::wheelEvent(QWheelEvent *event) {
    event->accept();
}
