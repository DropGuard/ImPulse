#pragma once

#include <QString>

// An alias, not a macro. This was #define'd, and the two #defines here
// (StdString and CharType) rewrote identifiers in every header included
// afterwards — a macro named CharType is gmock's own template parameter, so
// every TU that pulled in <gmock/gmock.h> behind this header failed to parse.
//
// The distinction is real and not decoration: on Windows fs::path must be
// built from wstring (UTF-16), because a narrow string is read in the ANSI
// code page and a non-ASCII path stops resolving. CharType had no users and
// is gone; StdString has eight, all in directorymanager.cpp.
#ifdef _WIN32
using StdString = std::wstring;
#else
using StdString = std::string;
#endif

int clamp(int x, int lower, int upper);
int probeOS();
StdString toStdString(const QString &str);
QString fromStdString(const StdString &str);

// Milliseconds since this binary's first startup checkpoint ran. The epoch is
// the FIRST call (main entry on every real startup), not the process's exec —
// loader and static-init time ahead of main is a linker property, not an app
// cost, and measuring it would put a constant in every number. Inline so the
// one static clock is shared by every checkpoint's translation unit.
#include <QElapsedTimer>
inline qint64 startupMs() {
    static const QElapsedTimer clock = [] {
        QElapsedTimer c;
        c.start();
        return c;
    }();
    return clock.elapsed();
}
