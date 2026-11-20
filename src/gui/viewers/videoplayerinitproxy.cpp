#include "videoplayerinitproxy.h"

#ifdef _QIMGV_PLAYER_PLUGIN
#define QIMGV_PLAYER_PLUGIN _QIMGV_PLAYER_PLUGIN
#else
#define QIMGV_PLAYER_PLUGIN ""
#endif

VideoPlayerInitProxy::VideoPlayerInitProxy(QWidget *parent) : VideoPlayer(parent), m_player(nullptr) {
    setAccessibleName("VideoPlayerInitProxy");
    setMouseTracking(true);
    m_layout.setContentsMargins(0, 0, 0, 0);
    setLayout(&m_layout);
    connect(settings, &Settings::settingsChanged, this, &VideoPlayerInitProxy::onSettingsChanged);
    m_libFile = QIMGV_PLAYER_PLUGIN;
#ifdef _WIN32
    m_libDirs << QApplication::applicationDirPath() + "/plugins";
#else
    // the plugin layout follows the app's name; the lib/qimgv variants stay
    // as fallbacks so installs that predate the rename keep playing video
    QDir impulseLib(QApplication::applicationDirPath() + "/../lib/impulse");
    QDir legacyLib(QApplication::applicationDirPath() + "/../lib/qimgv");
    m_libDirs << (impulseLib.makeAbsolute() ? impulseLib.path() : ".") << "/usr/lib/impulse" << "/usr/lib64/impulse"
              << (legacyLib.makeAbsolute() ? legacyLib.path() : ".") << "/usr/lib/qimgv" << "/usr/lib64/qimgv";
#endif
}

VideoPlayerInitProxy::~VideoPlayerInitProxy() {
}

void VideoPlayerInitProxy::onSettingsChanged() {
    if(!m_player)
        return;
    m_player->setMuted(!settings->playVideoSounds());
    m_player->setVideoUnscaled(!settings->expandImage());
}

VideoPlayer *VideoPlayerInitProxy::getPlayer() {
    return m_player;
}

bool VideoPlayerInitProxy::isInitialized() {
    return (m_player != nullptr);
}

inline bool VideoPlayerInitProxy::initPlayer() {
#ifndef USE_MPV
    return false;
#endif
    if(m_player)
        return true;

    QFileInfo pluginFile;
    for(const auto &dir : std::as_const(m_libDirs)) {
        pluginFile.setFile(dir + "/" + m_libFile);
        if(pluginFile.isFile() && pluginFile.isReadable()) {
            m_playerLib.setFileName(pluginFile.absoluteFilePath());
            break;
        }
    }
    if(m_playerLib.fileName().isEmpty()) {
        qDebug() << "Could not find" << m_libFile << "in the following directories:" << m_libDirs;
        return false;
    }

    // load lib
    typedef VideoPlayer *(*createPlayerWidgetFn)();
    createPlayerWidgetFn fn = reinterpret_cast<createPlayerWidgetFn>(m_playerLib.resolve("CreatePlayerWidget"));
    if(fn) {
        VideoPlayer *pl = fn();
        m_player = pl;
    }
    if(!m_player) {
        qDebug() << "Could not load:" << m_playerLib.fileName() << ". Wrong plugin version?";
        return false;
    }

    m_player->setMuted(!settings->playVideoSounds());
    m_player->setVideoUnscaled(!settings->expandImage());
    m_player->setVolume(settings->volume());

    m_player->setParent(this);
    m_layout.addWidget(m_player);
    m_player->hide();
    setFocusProxy(m_player);
    // signal-to-signal forwarding, pointer form: both endpoints are the same
    // four VideoPlayer signals (this class inherits them and must not
    // re-declare them — R6), and the compiler now checks every name/signature
    connect(m_player, &VideoPlayer::durationChanged, this, &VideoPlayer::durationChanged);
    connect(m_player, &VideoPlayer::positionChanged, this, &VideoPlayer::positionChanged);
    connect(m_player, &VideoPlayer::videoPaused, this, &VideoPlayer::videoPaused);
    connect(m_player, &VideoPlayer::playbackFinished, this, &VideoPlayer::playbackFinished);

    if(m_eventFilterObj)
        m_player->installEventFilter(m_eventFilterObj);

    return true;
}

bool VideoPlayerInitProxy::showVideo(QString file) {
    if(!initPlayer())
        return false;
    return m_player->showVideo(file);
}

void VideoPlayerInitProxy::seek(int pos) {
    if(!m_player)
        return;
    m_player->seek(pos);
}

void VideoPlayerInitProxy::seekRelative(int pos) {
    if(!m_player)
        return;
    m_player->seekRelative(pos);
}

void VideoPlayerInitProxy::pauseResume() {
    if(!m_player)
        return;
    m_player->pauseResume();
}

void VideoPlayerInitProxy::frameStep() {
    if(!m_player)
        return;
    m_player->frameStep();
}

void VideoPlayerInitProxy::frameStepBack() {
    if(!m_player)
        return;
    m_player->frameStepBack();
}

void VideoPlayerInitProxy::stop() {
    if(!m_player)
        return;
    m_player->stop();
}

void VideoPlayerInitProxy::setPaused(bool mode) {
    if(!m_player)
        return;
    m_player->setPaused(mode);
}

void VideoPlayerInitProxy::setMuted(bool mode) {
    if(!m_player)
        return;
    m_player->setMuted(mode);
}

bool VideoPlayerInitProxy::muted() {
    if(!m_player)
        return true;
    return m_player->muted();
}

void VideoPlayerInitProxy::volumeUp() {
    if(!m_player)
        return;
    m_player->volumeUp();
    settings->setVolume(m_player->volume());
}

void VideoPlayerInitProxy::volumeDown() {
    if(!m_player)
        return;
    m_player->volumeDown();
    settings->setVolume(m_player->volume());
}

void VideoPlayerInitProxy::setVolume(int vol) {
    if(!m_player)
        return;
    m_player->setVolume(vol);
}

int VideoPlayerInitProxy::volume() {
    if(!m_player)
        return 0;
    return m_player->volume();
}

void VideoPlayerInitProxy::setVideoUnscaled(bool mode) {
    if(!m_player)
        return;
    m_player->setVideoUnscaled(mode);
}

void VideoPlayerInitProxy::setLoopPlayback(bool mode) {
    if(!m_player)
        return;
    m_player->setLoopPlayback(mode);
}

void VideoPlayerInitProxy::show() {
    if(initPlayer()) {
        m_layout.removeWidget(m_errorLabel);
        m_player->show();
    } else if(!m_errorLabel) {
        m_errorLabel = new QLabel(this);
        m_errorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_errorLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        //errorLabel->setAlignment(Qt::AlignVCenter);
        QString errString = "Could not load " + m_libFile + " from:";
        for(const auto &path : std::as_const(m_libDirs))
            errString.append("\n" + path + "/");
        m_errorLabel->setText(errString);
        m_layout.addWidget(m_errorLabel);
    }
    VideoPlayer::show();
}

void VideoPlayerInitProxy::hide() {
    if(m_player)
        m_player->hide();
    VideoPlayer::hide();
}

void VideoPlayerInitProxy::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
}

void VideoPlayerInitProxy::installEventFilter(QObject *filterObj) {
    m_eventFilterObj = filterObj;
    if(m_player)
        m_player->installEventFilter(m_eventFilterObj);
}

void VideoPlayerInitProxy::removeEventFilter(QObject *filterObj) {
    if(m_player)
        m_player->removeEventFilter(filterObj);
}
