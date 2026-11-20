#include "floatingmessage.h"

#include <utility>
#include "ui_floatingmessage.h"

FloatingMessage::FloatingMessage(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), m_preferredPosition(FloatingWidgetPosition::BOTTOM), m_ui(new Ui::FloatingMessage) {
    m_ui->setupUi(this);
    m_hideDelay = 700;

    m_visibilityTimer.setSingleShot(true);
    m_visibilityTimer.setInterval(m_hideDelay);

    setFadeEnabled(true);
    setFadeDuration(300);

    setIcon(FloatingMessageIcon::NO_ICON);

    this->setAccessibleName("FloatingMessage");
    connect(&m_visibilityTimer, &QTimer::timeout, this, &FloatingMessage::hideAnimated);

    readSettings();

    connect(settings, &Settings::settingsChanged, this, &FloatingMessage::readSettings);

    if(parent)
        setContainerSize(parent->size());
}

FloatingMessage::~FloatingMessage() {
    delete m_ui;
}

void FloatingMessage::readSettings() {
    /*
    // don't interfere with the main panel
    if(settings->panelEnabled() && settings->panelPosition() == PanelHPosition::PANEL_BOTTOM) {
        preferredPosition = FloatingWidgetPosition::TOP;
    } else {
        preferredPosition = FloatingWidgetPosition::BOTTOM;
    }
    */
}

void FloatingMessage::showMessage(QString text, FloatingWidgetPosition anchor, FloatingMessageIcon icon, int duration) {
    setPosition(anchor);
    doShowMessage(std::move(text), icon, duration);
}

void FloatingMessage::showMessage(QString text, FloatingMessageIcon icon, int duration) {
    setPosition(m_preferredPosition);
    doShowMessage(std::move(text), icon, duration);
}

void FloatingMessage::doShowMessage(QString text, FloatingMessageIcon icon, int duration) {
    m_hideDelay = duration;
    setIcon(icon);
    setText(std::move(text));
    show();
}

void FloatingMessage::setText(const QString &text) {
    m_ui->textLabel->setText(text);
    text.isEmpty() ? m_ui->textLabel->hide() : m_ui->textLabel->show();
    recalculateGeometry();
    update();
}

void FloatingMessage::setIcon(FloatingMessageIcon icon) {
    switch(icon) {
    case FloatingMessageIcon::NO_ICON:
    case FloatingMessageIcon::ICON_WARNING:
    case FloatingMessageIcon::ICON_ERROR:
        //ui->iconLabel->setIconPath(":/res/icons/common/notifications/error16.png");
        m_ui->iconLabel->hide();
        break;
    case FloatingMessageIcon::ICON_DIRECTORY:
        m_ui->iconLabel->show();
        m_ui->iconLabel->setIconPath(":/res/icons/common/buttons/panel/folder16.png");
        break;
    case FloatingMessageIcon::ICON_LEFT_EDGE:
        m_ui->iconLabel->show();
        m_ui->iconLabel->setIconPath(":/res/icons/common/notifications/dir_start20.png");
        break;
    case FloatingMessageIcon::ICON_RIGHT_EDGE:
        m_ui->iconLabel->show();
        m_ui->iconLabel->setIconPath(":/res/icons/common/notifications/dir_end20.png");
        break;
    case FloatingMessageIcon::ICON_SUCCESS:
        m_ui->iconLabel->show();
        m_ui->iconLabel->setIconPath(":/res/icons/common/notifications/success16.png");
        break;
    }
}

void FloatingMessage::mousePressEvent(QMouseEvent *event) {
    Q_UNUSED(event)
}

// "blink" the widget; show then fade out immediately
void FloatingMessage::show() {
    m_visibilityTimer.stop();
    OverlayWidget::show();
    // fade out after delay
    m_visibilityTimer.setInterval(m_hideDelay);
    m_visibilityTimer.start();
}
