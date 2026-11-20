#include "mainpanel.h"

MainPanel::MainPanel(FloatingWidgetContainer *parent) : SlidePanel(parent) {
    // buttons stuff
    m_buttonsWidget.setAccessibleName("panelButtonsWidget");
    m_openButton = new ActionButton("open", ":res/icons/common/buttons/panel/open20.png", 30, this);
    m_openButton->setAccessibleName("ButtonSmall");
    m_openButton->setTriggerMode(TriggerMode::PressTrigger);
    m_settingsButton = new ActionButton("openSettings", ":res/icons/common/buttons/panel/settings20.png", 30, this);
    m_settingsButton->setAccessibleName("ButtonSmall");
    m_settingsButton->setTriggerMode(TriggerMode::PressTrigger);
    m_exitButton = new ActionButton("exit", ":res/icons/common/buttons/panel/close16.png", 30, this);
    m_exitButton->setAccessibleName("ButtonSmall");
    m_exitButton->setTriggerMode(TriggerMode::PressTrigger);
    m_folderViewButton = new ActionButton("folderView", ":res/icons/common/buttons/panel/folderview20.png", 30, this);
    m_folderViewButton->setAccessibleName("ButtonSmall");
    m_folderViewButton->setTriggerMode(TriggerMode::PressTrigger);
    m_pinButton = new ActionButton("", ":res/icons/common/buttons/panel/pin-panel20.png", 30, this);
    m_pinButton->setAccessibleName("ButtonSmall");
    m_pinButton->setTriggerMode(TriggerMode::PressTrigger);
    m_pinButton->setCheckable(true);
    connect(m_pinButton, &ActionButton::toggled, this, &MainPanel::onPinClicked);

    // owned by the widget it lays out: installed as a member subobject on another
    // member, buttonsWidget's ~QObject deleted it before MainPanel's own member
    // destruction got there (members die in reverse declaration order) — a free of
    // an address that was never allocated. The pointer here only observes.
    m_buttonsLayout = new QVBoxLayout(&m_buttonsWidget);
    m_buttonsLayout->setDirection(QBoxLayout::BottomToTop);
    m_buttonsLayout->setSpacing(0);
    m_buttonsLayout->addWidget(m_settingsButton);
    m_buttonsLayout->addWidget(m_openButton);
    m_buttonsLayout->addStretch(0);
    m_buttonsLayout->addWidget(m_pinButton);
    m_buttonsLayout->addWidget(m_folderViewButton);
    m_buttonsLayout->addWidget(m_exitButton);

    layout()->addWidget(&m_buttonsWidget);

    m_thumbnailStrip = new ThumbnailStripProxy(this);
    setWidget(m_thumbnailStrip);

    readSettings();
    // deliberately NOT connected to Settings::settingsChanged. The bus reaches
    // this method already, in three greppable steps: documentwidget.cpp:38
    // subscribes DocumentWidget::readSettings to Settings::settingsChanged,
    // documentwidget.cpp:50 calls mainPanel->readSettings(), and this method
    // calls thumbnailStrip->readSettings() below. A subscription here would run
    // the same refresh twice per settings change. The chain is now pinned at
    // runtime by core_nav_tests:
    // settingsBusReachesTheThumbnailStripThroughThePanelChain, which reaches
    // the strip through MainWindow's own graph (mainwindow.cpp:71-73 builds
    // ViewerWidget, InfoBarProxy and DocumentWidget at startup), so no fixture
    // had to be built to prove it.
}

void MainPanel::onPinClicked() {
    bool mode = !settings->panelPinned();
    m_pinButton->setChecked(mode);
    settings->setPanelPinned(mode);
    emit pinned(mode);
}

void MainPanel::setPosition(PanelPosition p) {
    SlidePanel::setPosition(p);
    switch(p) {
    case PANEL_TOP:
        m_buttonsLayout->setDirection(QBoxLayout::BottomToTop);
        layout()->setContentsMargins(0, 0, 0, 1);
        m_buttonsLayout->setContentsMargins(4, 0, 0, 0);
        break;
    case PANEL_BOTTOM:
        m_buttonsLayout->setDirection(QBoxLayout::BottomToTop);
        layout()->setContentsMargins(0, 3, 0, 0);
        m_buttonsLayout->setContentsMargins(4, 0, 0, 0);
        break;
    case PANEL_LEFT:
        m_buttonsLayout->setDirection(QBoxLayout::LeftToRight);
        layout()->setContentsMargins(0, 0, 1, 0);
        m_buttonsLayout->setContentsMargins(0, 0, 0, 4);
        break;
    case PANEL_RIGHT:
        m_buttonsLayout->setDirection(QBoxLayout::LeftToRight);
        layout()->setContentsMargins(1, 0, 0, 0);
        m_buttonsLayout->setContentsMargins(0, 0, 0, 4);
        break;
    }
    recalculateGeometry();
}

void MainPanel::setExitButtonEnabled(bool mode) {
    m_exitButton->setHidden(!mode);
}

ThumbnailStripProxy *MainPanel::getThumbnailStrip() {
    return m_thumbnailStrip;
}

void MainPanel::setupThumbnailStrip() {
    m_thumbnailStrip->init();
    // adjust size & position
    readSettings();
}

QSize MainPanel::sizeHint() const {
    if(!m_thumbnailStrip->isInitialized())
        return QSize(0, 0);
    // item size + spacing + scrollbar + border
    switch(settings->panelPosition()) {
    case PANEL_TOP: return QSize(width(), m_thumbnailStrip->itemSize().height() + 16);
    case PANEL_BOTTOM: return QSize(width(), m_thumbnailStrip->itemSize().height() + 16 + 3);
    case PANEL_LEFT:
    case PANEL_RIGHT: return QSize(m_thumbnailStrip->itemSize().width() + 16, height());
    default: return QSize(0, 0);
    }
}

void MainPanel::readSettings() {
    auto newPos = settings->panelPosition();
    if(newPos == PANEL_TOP || newPos == PANEL_BOTTOM) {
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        int h = sizeHint().height();
        if(h)
            setFixedHeight(h);
        setFixedWidth(QWIDGETSIZE_MAX);
    } else {
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        int w = sizeHint().width();
        if(w)
            setFixedWidth(w);
        setFixedHeight(QWIDGETSIZE_MAX);
    }
    m_thumbnailStrip->readSettings();
    setPosition(newPos);
    m_pinButton->setChecked(settings->panelPinned());
}

// draw separator line at bottom or top
void MainPanel::paintEvent(QPaintEvent *event) {
    QWidget::paintEvent(event);
    // borders
    QPainter p(this);
    p.setPen(settings->colorScheme().folderview_hc);
    switch(m_position) {
    case PANEL_TOP: p.drawLine(rect().bottomLeft(), rect().bottomRight()); break;
    case PANEL_BOTTOM:
        p.fillRect(rect().left(), rect().top(), width(), 3, settings->colorScheme().folderview);
        p.drawLine(rect().topLeft(), rect().topRight());
        break;
    case PANEL_LEFT: p.drawLine(rect().topRight(), rect().bottomRight()); break;
    case PANEL_RIGHT: p.drawLine(rect().topLeft(), rect().bottomLeft()); break;
    }
}
