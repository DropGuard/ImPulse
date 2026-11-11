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

    QString path() const {
        return dir.path();
    }

  private:
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
        QCoreApplication::setApplicationName(fresh);
        ::settings = new Settings();
    }

    ~FreshSettings() {
        delete ::settings;
        ::settings = saved;
    }

    FreshSettings(const FreshSettings &) = delete;
    FreshSettings &operator=(const FreshSettings &) = delete;

  private:
    inline static int counter = 0;
    Settings *saved = nullptr;
};
