#pragma once

// Shared fixtures for the C++ test binaries. These exist because the two
// singletons tests always fight are Settings (a process-wide config backed by
// QSettings) and, previously, the EngineBridge — the bridge became
// constructible per-test, and these helpers cover the rest.

#include <QTest>
#include <QTemporaryDir>
#include <QCoreApplication>

#include <QImage>
#include <QColor>

#include "settings.h"

// Redirects HOME and the QSettings identity into a throwaway directory for
// the lifetime of the object. Construct it BEFORE anything touches Settings —
// QStandardPaths and QSettings both resolve through these values lazily, so
// the earlier the better.
// value-returning file helpers for tests: they report through qFail (a bare
// return inside these would be a compile error, and three copies of that
// mistake across two suites is the reason this lives here)
inline QString writeTestFile(const QString &path, const QByteArray &content) {
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly)) {
        QTest::qFail(qPrintable(QStringLiteral("cannot create %1: %2").arg(path, f.errorString())), __FILE__, __LINE__);
        return {};
    }
    f.write(content);
    return path;
}

inline QByteArray readTestFile(const QString &path) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) {
        QTest::qFail(qPrintable(QStringLiteral("cannot read %1: %2").arg(path, f.errorString())), __FILE__, __LINE__);
        return {};
    }
    return f.readAll();
}

// writes <dir>/<name> with the given content, reports through qFail and
// returns the full path (the convention shared by both suites' fixtures)
inline QString makeTxt(const QString &dir, const QString &name, const char *content) {
    return writeTestFile(QDir(dir).filePath(name), content);
}

// writes an image file of the given size/format, solid fill. Reports through
// qFail and still returns false so QVERIFY-style callers can fail their own
// frame.
inline bool makeImageFile(const QString &path, const QSize &size, const char *format = "PNG",
                          QColor fill = QColor(Qt::red)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(fill);
    if(!img.save(path, format)) {
        QTest::qFail(qPrintable(QStringLiteral("cannot create %1").arg(path)), __FILE__, __LINE__);
        return false;
    }
    return true;
}

class IsolatedHome {
  public:
    IsolatedHome() {
        dir.setAutoRemove(true);
        Q_ASSERT(dir.isValid());
        // What was there before: restored on destruction, because a fixture that
        // only writes is a fixture that leaks. Today every use is a suite member
        // living as long as the process, so the restore never fires in practice
        // -- which is the other reason to have it: the day someone scopes one of
        // these to a function (the way FreshSettings is used), the code after it
        // must not keep running against a directory that was just deleted.
        savedHome = qgetenv("HOME");
        savedCache = qgetenv("XDG_CACHE_HOME");
        savedConfig = qgetenv("XDG_CONFIG_HOME");
        savedOrg = QCoreApplication::organizationName();
        savedApp = QCoreApplication::applicationName();
        hadHome = qEnvironmentVariableIsSet("HOME");
        hadCache = qEnvironmentVariableIsSet("XDG_CACHE_HOME");
        hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("HOME", dir.path().toUtf8());
        // HOME alone no longer isolates the cache: the thumbnail store lives in
        // the shared freedesktop location, and QStandardPaths prefers
        // XDG_CACHE_HOME over $HOME/.cache. On a machine where the developer set
        // it (a common KDE setup), a test that reached the real store would read
        // and DELETE the user's thumbnails. Pointing both at the fixture keeps
        // the isolation true regardless of the caller's session.
        qputenv("XDG_CACHE_HOME", QDir(dir.path()).filePath("cache").toUtf8());
        qputenv("XDG_CONFIG_HOME", QDir(dir.path()).filePath("config").toUtf8());
        QCoreApplication::setOrganizationName("ImPulse");
        QCoreApplication::setApplicationName("impulse");
    }

    ~IsolatedHome() {
        restore("HOME", savedHome, hadHome);
        restore("XDG_CACHE_HOME", savedCache, hadCache);
        restore("XDG_CONFIG_HOME", savedConfig, hadConfig);
        QCoreApplication::setOrganizationName(savedOrg);
        QCoreApplication::setApplicationName(savedApp);
    }

    IsolatedHome(const IsolatedHome &) = delete;
    IsolatedHome &operator=(const IsolatedHome &) = delete;

    QString path() const {
        return dir.path();
    }

  private:
    static void restore(const char *name, const QByteArray &value, bool wasSet) {
        if(wasSet)
            qputenv(name, value);
        else
            qunsetenv(name);
    }

    QByteArray savedHome, savedCache, savedConfig;
    QString savedOrg, savedApp;
    bool hadHome = false, hadCache = false, hadConfig = false;
    QTemporaryDir dir;
};

// Swaps the global `settings` pointer to a brand-new Settings instance bound
// to a unique config identity, so a test's writes are invisible to every
// other test and to the original instance. The original pointer (and its
// live state) is restored on destruction.
class FreshSettings {
  public:
    FreshSettings() {
        saved = ::settings;
        // a unique application name means a config file nothing else reads or
        // has cached: the fresh instance starts from defaults, deterministically
        const QString fresh =
            QStringLiteral("qimgv-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(++counter, 0, 16);
        savedApp = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(fresh);
        ::settings = new Settings();
    }

    ~FreshSettings() {
        delete ::settings;
        ::settings = saved;
        // the name is part of the identity this fixture swaps: leaving it at the
        // unique test name meant the next QSettings wrote to a config path that
        // nothing else in the run expects -- IsolatedHome, in particular,
        // assumes applicationName is the production one.
        QCoreApplication::setApplicationName(savedApp);
    }

    FreshSettings(const FreshSettings &) = delete;
    FreshSettings &operator=(const FreshSettings &) = delete;

  private:
    inline static int counter = 0;
    Settings *saved = nullptr;
    QString savedApp;
};
