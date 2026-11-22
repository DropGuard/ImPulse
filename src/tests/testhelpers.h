#pragma once

// Shared fixtures for the C++ test binaries. These exist because the two
// singletons tests always fight are Settings (a process-wide config backed by
// QSettings) and, previously, the EngineBridge — the bridge became
// constructible per-test, and these helpers cover the rest.

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QCoreApplication>

#include <QImage>
#include <QColor>

#include "settings.h"

// Redirects HOME and the QSettings identity into a throwaway directory for
// the lifetime of the object. Construct it BEFORE anything touches Settings —
// QStandardPaths and QSettings both resolve through these values lazily, so
// the earlier the better.
// value-returning file helpers for tests. A helper cannot assert for its
// caller, and QTest::qFail reports to a result object that only exists inside
// QTest::qExec — in a gtest translation unit it is silent. qFatal names the
// path and aborts the (per-test) process instead.
inline QString writeTestFile(const QString &path, const QByteArray &content) {
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly)) {
        qFatal("cannot create %s: %s", qPrintable(path), qPrintable(f.errorString()));
        return {};
    }
    f.write(content);
    return path;
}

inline QByteArray readTestFile(const QString &path) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) {
        qFatal("cannot read %s: %s", qPrintable(path), qPrintable(f.errorString()));
        return {};
    }
    return f.readAll();
}

// writes <dir>/<name> with the given content and returns the full path
inline QString makeTxt(const QString &dir, const QString &name, const char *content) {
    return writeTestFile(QDir(dir).filePath(name), content);
}

// writes an image file of the given size/format, solid fill. Aborts on a
// failed encode: the callers' own asserts would otherwise compare against a
// file that was never written.
inline bool makeImageFile(const QString &path, const QSize &size, const char *format = "PNG",
                          QColor fill = QColor(Qt::red)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(fill);
    if(!img.save(path, format))
        qFatal("cannot create %s", qPrintable(path));
    return true;
}

class IsolatedHome {
  public:
    IsolatedHome() {
        m_dir.setAutoRemove(true);
        Q_ASSERT(m_dir.isValid());
        // What was there before: restored on destruction, because a fixture that
        // only writes is a fixture that leaks. Today every use is a suite member
        // living as long as the process, so the restore never fires in practice
        // -- which is the other reason to have it: the day someone scopes one of
        // these to a function (the way FreshSettings is used), the code after it
        // must not keep running against a directory that was just deleted.
        m_savedHome = qgetenv("HOME");
        m_savedCache = qgetenv("XDG_CACHE_HOME");
        m_savedConfig = qgetenv("XDG_CONFIG_HOME");
        m_savedOrg = QCoreApplication::organizationName();
        m_savedApp = QCoreApplication::applicationName();
        m_hadHome = qEnvironmentVariableIsSet("HOME");
        m_hadCache = qEnvironmentVariableIsSet("XDG_CACHE_HOME");
        m_hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("HOME", m_dir.path().toUtf8());
        // HOME alone no longer isolates the cache: the thumbnail store lives in
        // the shared freedesktop location, and QStandardPaths prefers
        // XDG_CACHE_HOME over $HOME/.cache. On a machine where the developer set
        // it (a common KDE setup), a test that reached the real store would read
        // and DELETE the user's thumbnails. Pointing both at the fixture keeps
        // the isolation true regardless of the caller's session.
        qputenv("XDG_CACHE_HOME", QDir(m_dir.path()).filePath("cache").toUtf8());
        qputenv("XDG_CONFIG_HOME", QDir(m_dir.path()).filePath("config").toUtf8());
        QCoreApplication::setOrganizationName("ImPulse");
        QCoreApplication::setApplicationName("impulse");
    }

    ~IsolatedHome() {
        restore("HOME", m_savedHome, m_hadHome);
        restore("XDG_CACHE_HOME", m_savedCache, m_hadCache);
        restore("XDG_CONFIG_HOME", m_savedConfig, m_hadConfig);
        QCoreApplication::setOrganizationName(m_savedOrg);
        QCoreApplication::setApplicationName(m_savedApp);
    }

    IsolatedHome(const IsolatedHome &) = delete;
    IsolatedHome &operator=(const IsolatedHome &) = delete;

    QString path() const {
        return m_dir.path();
    }

  private:
    static void restore(const char *name, const QByteArray &value, bool wasSet) {
        if(wasSet)
            qputenv(name, value);
        else
            qunsetenv(name);
    }

    QByteArray m_savedHome, m_savedCache, m_savedConfig;
    QString m_savedOrg, m_savedApp;
    bool m_hadHome = false, m_hadCache = false, m_hadConfig = false;
    QTemporaryDir m_dir;
};

// A QSettings handle on the SAME conf store the Settings instance reads:
// tests that write junk through one backend and expect the accessors to
// answer from it build the handle here instead of re-deriving the backend
// polarity per file. Construct AFTER FreshSettings — the store follows the
// redirected env and the fresh applicationName, exactly like the handles the
// old per-site Q_OS_WIN copies built. The backend decision itself is
// production's (Settings::portableConfDir); the old copies asked !Q_OS_WIN
// while production asked __linux__||__FreeBSD__ — inverted words for the
// same question, which on any third platform would have written the junk
// into a store production never opens.
inline QSettings confStoreHandle(const QString &fileStem) {
    const QString confDir = Settings::portableConfDir();
    if(confDir.isEmpty())
        return QSettings(QCoreApplication::organizationName(), fileStem);
    return QSettings(confDir + "/" + fileStem + ".ini", QSettings::IniFormat);
}

// Swaps the global `settings` pointer to a brand-new Settings instance bound
// to a unique config identity, so a test's writes are invisible to every
// other test and to the original instance. The original pointer (and its
// live state) is restored on destruction.
class FreshSettings {
  public:
    FreshSettings() {
        m_saved = ::settings;
        // a unique application name means a config file nothing else reads or
        // has cached: the fresh instance starts from defaults, deterministically
        const QString fresh =
            QStringLiteral("qimgv-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(++counter, 0, 16);
        m_savedApp = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(fresh);
        // Windows portable mode reads conf and cache from the exe's directory
        // and ignores the applicationName swap entirely — this env redirect is
        // what makes the isolation actually hold on that platform (the same
        // trick the XDG vars above are for on Linux).
        m_savedConfEnv = qgetenv("QIMGV_CONF_DIR");
        m_savedCacheEnv = qgetenv("QIMGV_CACHE_DIR");
        const QString confFresh = m_dir.path() + "/conf";
        const QString cacheFresh = m_dir.path() + "/cache";
        QDir().mkpath(confFresh);
        QDir().mkpath(cacheFresh);
        qputenv("QIMGV_CONF_DIR", confFresh.toUtf8());
        qputenv("QIMGV_CACHE_DIR", cacheFresh.toUtf8());
        ::settings = new Settings();
    }

    ~FreshSettings() {
        delete ::settings;
        ::settings = m_saved;
        // the name is part of the identity this fixture swaps: leaving it at the
        // unique test name meant the next QSettings wrote to a config path that
        // nothing else in the run expects -- IsolatedHome, in particular,
        // assumes applicationName is the production one.
        QCoreApplication::setApplicationName(m_savedApp);
        if(m_savedConfEnv.isEmpty())
            qunsetenv("QIMGV_CONF_DIR");
        else
            qputenv("QIMGV_CONF_DIR", m_savedConfEnv);
        if(m_savedCacheEnv.isEmpty())
            qunsetenv("QIMGV_CACHE_DIR");
        else
            qputenv("QIMGV_CACHE_DIR", m_savedCacheEnv);
    }

    FreshSettings(const FreshSettings &) = delete;
    FreshSettings &operator=(const FreshSettings &) = delete;

  private:
    inline static int counter = 0;
    Settings *m_saved = nullptr;
    QString m_savedApp;
    QByteArray m_savedConfEnv, m_savedCacheEnv;
    QTemporaryDir m_dir;
};
