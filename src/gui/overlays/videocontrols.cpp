#include "videocontrols.h"
#include "ui_videocontrols.h"

VideoControls::VideoControls(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), m_ui(new Ui::VideoControls), m_mode(PLAYBACK_ANIMATION) {
    m_ui->setupUi(this);
    this->setAttribute(Qt::WA_NoMousePropagation, true);
    hide();
    m_ui->pauseButton->setIconPath(":res/icons/common/buttons/videocontrols/play24.png");
    m_ui->pauseButton->setAction("pauseVideo");
    m_ui->prevFrameButton->setIconPath(":res/icons/common/buttons/videocontrols/skip-backwards24.png");
    m_ui->prevFrameButton->setAction("frameStepBack");
    m_ui->nextFrameButton->setIconPath(":res/icons/common/buttons/videocontrols/skip-forward24.png");
    m_ui->nextFrameButton->setAction("frameStep");
    m_ui->muteButton->setIconPath(":/res/icons/common/buttons/videocontrols/mute-on24.png");
    m_ui->muteButton->setAction("toggleMute");

    m_lastPosition = -1;

    readSettings();
    connect(settings, &Settings::settingsChanged, this, &VideoControls::readSettings);

    connect(m_ui->seekBar, &VideoSlider::sliderMovedX, this, &VideoControls::seek);

    if(parent)
        setContainerSize(parent->size());
}

void VideoControls::readSettings() {
    if(settings->panelEnabled() && settings->panelPosition() == PanelPosition::PANEL_BOTTOM)
        setPosition(FloatingWidgetPosition::TOP);
    else
        setPosition(FloatingWidgetPosition::BOTTOM);
}

VideoControls::~VideoControls() {
    delete m_ui;
}

void VideoControls::setMode(PlaybackMode _mode) {
    m_mode = _mode;
    m_ui->muteButton->setVisible((m_mode == PLAYBACK_VIDEO));
}

void VideoControls::setPlaybackDuration(int duration) {
    QString durationStr;
    if(m_mode == PLAYBACK_VIDEO) {
        int _time = duration;
        int hours = _time / 3600;
        _time -= hours * 3600;
        int minutes = _time / 60;
        int seconds = _time - minutes * 60;
        durationStr =
            QString("%1").arg(minutes, 2, 10, QChar('0')) + ":" + QString("%1").arg(seconds, 2, 10, QChar('0'));
        if(hours)
            durationStr.prepend(QString("%1").arg(hours, 2, 10, QChar('0')) + ":");
    } else {
        durationStr = QString::number(duration);
    }
    m_ui->seekBar->setRange(0, duration - 1);
    m_ui->durationLabel->setText(durationStr);
    m_ui->positionLabel->setText(durationStr);
    recalculateGeometry();
    m_ui->positionLabel->setText("");
}

void VideoControls::setPlaybackPosition(int pos) {
    if(pos == m_lastPosition)
        return;
    QString positionStr;
    if(m_mode == PLAYBACK_VIDEO) {
        int _time = pos;
        int hours = _time / 3600;
        _time -= hours * 3600;
        int minutes = _time / 60;
        int seconds = _time - minutes * 60;
        positionStr =
            QString("%1").arg(minutes, 2, 10, QChar('0')) + ":" + QString("%1").arg(seconds, 2, 10, QChar('0'));
        if(hours)
            positionStr.prepend(QString("%1").arg(hours, 2, 10, QChar('0')) + ":");
    } else {
        positionStr = QString::number(pos + 1);
    }
    m_ui->positionLabel->setText(positionStr);
    m_ui->seekBar->blockSignals(true);
    m_ui->seekBar->setValue(pos);
    m_ui->seekBar->blockSignals(false);
    m_lastPosition = pos;
}

void VideoControls::onPlaybackPaused(bool paused) {
    if(paused)
        m_ui->pauseButton->setIconPath(":res/icons/common/buttons/videocontrols/play24.png");
    else
        m_ui->pauseButton->setIconPath(":res/icons/common/buttons/videocontrols/pause24.png");
}

void VideoControls::onVideoMuted(bool muted) {
    if(muted)
        m_ui->muteButton->setIconPath(":res/icons/common/buttons/videocontrols/mute-on24.png");
    else
        m_ui->muteButton->setIconPath(":res/icons/common/buttons/videocontrols/mute-off24.png");
}
