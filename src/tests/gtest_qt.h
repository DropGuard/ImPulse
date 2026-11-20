#pragma once

// The GoogleTest half of the test suite's vocabulary.
//
// Qt6::Test stays linked — QSignalSpy, QTest::qWait, qWaitForWindowExposed and
// keyClick have no gtest equivalent — but its ASSERTION macros must never be
// used in a gtest translation unit. That is not a style rule:
//
//   QVERIFY / QCOMPARE / QTRY_* / QFAIL write their verdict to QTest's own
//   result object, which only exists inside QTest::qExec. Under a gtest main
//   that object is absent, so a failing QVERIFY reports nothing and executes
//   a bare `return;` — which in a TEST_F body is an ordinary return, and gtest
//   records the case as PASSED.
//
// Measured, not assumed: QVERIFY(false), QTRY_VERIFY_WITH_TIMEOUT(false, 200)
// and QTest::qFail(...) each printed [ OK ] and exited 0 inside a TEST_F.
//
// The poison below makes that class of silent green a compile error. It is the
// only mechanism here that cannot be forgotten: a file that includes <QTest>
// for its tools and then reaches for an assertion dies at compile time, with
// the macro named.
// The specific headers, not the module: clazy's no-module-include names the
// module form, and only these facilities are used here (qWaitFor and
// qWaitForWindow* live in qtestcase.h, the input synthesis in the other two).
#include <QtTest/qtestcase.h>
#include <QtTest/qtestkeyboard.h>
#include <QtTest/qtestmouse.h>

#undef QVERIFY
#undef QVERIFY2
#undef QCOMPARE
#undef QFAIL
#undef QSKIP
#undef QEXPECT_FAIL
#undef QTRY_VERIFY
#undef QTRY_VERIFY2
#undef QTRY_COMPARE
#undef QTRY_VERIFY_WITH_TIMEOUT
#undef QTRY_VERIFY2_WITH_TIMEOUT
#undef QTRY_COMPARE_WITH_TIMEOUT

#pragma GCC poison QVERIFY QVERIFY2 QCOMPARE QFAIL QSKIP QEXPECT_FAIL QTRY_VERIFY QTRY_VERIFY2 QTRY_COMPARE QTRY_VERIFY_WITH_TIMEOUT QTRY_VERIFY2_WITH_TIMEOUT QTRY_COMPARE_WITH_TIMEOUT

#include <gtest/gtest.h>

#include <QString>
#include <ostream>

// QString has no operator<< for std::ostream, so a bare EXPECT_EQ on one prints
// "N-byte object" — the one value a failing comparison most needs to show.
inline void PrintTo(const QString &s, std::ostream *os) {
    *os << '"' << s.toStdString() << '"';
}

// The asynchronous vocabulary, in gtest terms. QTest::qWaitFor pumps the event
// loop until the predicate holds or the deadline expires and returns whether it
// did — unlike QTRY_*, it reports nothing on its own, so the assertion is ours.
//
// ASSERT_* rather than EXPECT_*: a timed-out wait means the state the rest of
// the test observes never arrived, so continuing would assert against whatever
// happened to be there.
#define ASSERT_TRY_TRUE_TIMEOUT(expr, timeout_ms)                                                                      \
    do {                                                                                                               \
        const bool _qt_ok = QTest::qWaitFor(                                                                           \
            [&]() {                                                                                                    \
                return bool(expr);                                                                                     \
            },                                                                                                         \
            (timeout_ms));                                                                                             \
        ASSERT_TRUE(_qt_ok) << "timed out after " << (timeout_ms) << "ms waiting for: " #expr;                         \
    } while(false)

#define ASSERT_TRY_TRUE(expr) ASSERT_TRY_TRUE_TIMEOUT(expr, 5000)

// Polls equality, then asserts the settled value: the wait alone cannot say
// WHICH side was wrong, and the comparison is what a failure needs to show.
#define ASSERT_TRY_EQ_TIMEOUT(actual, expected, timeout_ms)                                                            \
    do {                                                                                                               \
        (void)QTest::qWaitFor(                                                                                         \
            [&]() {                                                                                                    \
                return (actual) == (expected);                                                                         \
            },                                                                                                         \
            (timeout_ms));                                                                                             \
        ASSERT_EQ((actual), (expected)) << "timed out after " << (timeout_ms) << "ms";                                 \
    } while(false)

#define ASSERT_TRY_EQ(actual, expected) ASSERT_TRY_EQ_TIMEOUT(actual, expected, 5000)

// The non-fatal twins, for a wait whose failure the test can still learn from.
#define EXPECT_TRY_TRUE_TIMEOUT(expr, timeout_ms)                                                                      \
    do {                                                                                                               \
        const bool _qt_ok = QTest::qWaitFor(                                                                           \
            [&]() {                                                                                                    \
                return bool(expr);                                                                                     \
            },                                                                                                         \
            (timeout_ms));                                                                                             \
        EXPECT_TRUE(_qt_ok) << "timed out after " << (timeout_ms) << "ms waiting for: " #expr;                         \
    } while(false)

#define EXPECT_TRY_EQ_TIMEOUT(actual, expected, timeout_ms)                                                            \
    do {                                                                                                               \
        (void)QTest::qWaitFor(                                                                                         \
            [&]() {                                                                                                    \
                return (actual) == (expected);                                                                         \
            },                                                                                                         \
            (timeout_ms));                                                                                             \
        EXPECT_EQ((actual), (expected)) << "timed out after " << (timeout_ms) << "ms";                                 \
    } while(false)
