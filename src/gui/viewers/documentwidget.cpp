#include "documentwidget.h"

DocumentWidget::DocumentWidget(ViewerWidget *viewWidget, InfoBarProxy *infoBar, QWidget *parent)
    : FloatingWidgetContainer(parent), m_mainPanel(nullptr), m_panelPinned(false), m_panelEnabled(false),
      m_panelFullscreenOnly(false), m_avoidPanelFlag(false), m_isFullscreen(false), m_interactionEnabled(false),
      m_allowPanelInit(false) {
    m_layoutRoot = new QVBoxLayout();
    m_layoutRoot->setContentsMargins(0, 0, 0, 0);
    m_layoutRoot->setSpacing(0);
    m_layout = new QBoxLayout(QBoxLayout::LeftToRight);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layoutRoot->addLayout(m_layout);
    setLayout(m_layoutRoot);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    m_viewWidget = viewWidget;
    m_viewWidget->setParent(this);
    m_layout->addWidget(m_viewWidget);
    m_viewWidget->show();
    m_infoBar = infoBar;
    m_infoBar->setParent(this);
    m_layoutRoot->addWidget(m_infoBar);
    setFocusProxy(m_viewWidget);

    setInteractionEnabled(true);

    m_mainPanel = new MainPanel(this);
    connect(m_mainPanel, &MainPanel::pinned, this, &DocumentWidget::setPanelPinned);

    connect(settings, &Settings::settingsChanged, this, &DocumentWidget::readSettings);
    readSettings();
}

ViewerWidget *DocumentWidget::viewWidget() {
    return m_viewWidget;
}

void DocumentWidget::readSettings() {
    setPanelEnabled(settings->panelEnabled());
    m_panelFullscreenOnly = settings->panelFullscreenOnly();
    setPanelPinned(settings->panelPinned());
    m_mainPanel->readSettings();
}

void DocumentWidget::onFullscreenModeChanged(bool mode) {
    if(settings->panelPosition() == PANEL_TOP || settings->panelPosition() == PANEL_RIGHT)
        m_mainPanel->setExitButtonEnabled(mode);
    else
        m_mainPanel->setExitButtonEnabled(false);
    m_isFullscreen = mode;
}

void DocumentWidget::setPanelPinned(bool mode) {
    if(!m_panelEnabled)
        return;
    if(!mode) { // unpin
        if(m_panelPinned)
            m_layout->removeWidget(m_mainPanel);
        m_mainPanel->setLayoutManaged(false);
    } else { // pin
        m_layout->insertWidget(1, m_mainPanel);
        switch(settings->panelPosition()) {
        case PANEL_TOP: m_layout->setDirection(QBoxLayout::BottomToTop); break;
        case PANEL_BOTTOM: m_layout->setDirection(QBoxLayout::TopToBottom); break;
        case PANEL_LEFT: m_layout->setDirection(QBoxLayout::RightToLeft); break;
        case PANEL_RIGHT: m_layout->setDirection(QBoxLayout::LeftToRight); break;
        }
        m_mainPanel->setLayoutManaged(true);
        m_mainPanel->show();
    }
    m_panelPinned = mode;
}

bool DocumentWidget::panelPinned() {
    return m_panelPinned;
}

ThumbnailStripProxy *DocumentWidget::thumbPanel() {
    return m_mainPanel->getThumbnailStrip();
}

void DocumentWidget::hideFloatingPanel() {
    hideFloatingPanel(false);
}

void DocumentWidget::hideFloatingPanel(bool animated) {
    Q_UNUSED(animated)
    if(!m_panelPinned)
        m_mainPanel->hideAnimated();
}

void DocumentWidget::setPanelEnabled(bool mode) {
    m_panelEnabled = mode;
    if(!mode)
        m_mainPanel->hide();
    else
        setupMainPanel();
}

bool DocumentWidget::panelEnabled() {
    return m_panelEnabled;
}

void DocumentWidget::allowPanelInit() {
    m_allowPanelInit = true;
}

void DocumentWidget::setupMainPanel() {
    if(m_panelEnabled && m_allowPanelInit)
        m_mainPanel->setupThumbnailStrip();
}

void DocumentWidget::setInteractionEnabled(bool mode) {
    m_interactionEnabled = mode;
    if(!mode && !m_panelPinned)
        m_mainPanel->hide();
}

void DocumentWidget::mouseMoveEvent(QMouseEvent *event) {
    event->ignore();
    if(m_panelPinned)
        return;
    // ignore if we are doing something with the mouse (zoom / drag)
    if(event->buttons() != Qt::NoButton) {
        if(m_mainPanel->triggerRect().contains(event->pos()))
            m_avoidPanelFlag = true;
        return;
    }
    // show on hover event
    if(m_panelEnabled && (m_isFullscreen || !m_panelFullscreenOnly)) {
        if(m_mainPanel->triggerRect().contains(event->pos()) && !m_avoidPanelFlag) {
            m_mainPanel->show();
        }
    }
    // fade out on leave event
    if(!m_mainPanel->isHidden()) {
        // leaveEvent which misfires on HiDPI (rounding error somewhere?)
        // add a few px of buffer area to avoid bugs
        // it still fcks up Fitts law as the buttons are not receiving hover on screen border

        // alright this also only works when in root window. sad.
        if(!m_mainPanel->triggerRect().adjusted(-8, -8, 8, 8).contains(event->pos())) {
            m_mainPanel->hideAnimated();
        }
    }
    if(!m_mainPanel->triggerRect().contains(event->pos()))
        m_avoidPanelFlag = false;
}

void DocumentWidget::enterEvent(QEnterEvent *event) {
    QWidget::enterEvent(event);
    if(!m_interactionEnabled)
        return;
    // we can't track move events outside the window (without additional timer),
    // so just hook the panel event here
    if(!m_panelPinned && m_panelEnabled && (m_isFullscreen || !m_panelFullscreenOnly)) {
        if(m_mainPanel->triggerRect().contains(mapFromGlobal(cursor().pos())) && !m_avoidPanelFlag)
            m_mainPanel->show();
    }
}

void DocumentWidget::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    //qDebug() << cursor().pos() << this->rect();
    // this misfires on hidpi.
    //instead do the panel hiding in MW::leaveEvent  (it works properly in root window)
    //mainPanel->hide();
    m_avoidPanelFlag = false;
}
