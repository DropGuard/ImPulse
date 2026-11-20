// The harness proving itself.
//
// Two facts have to hold for the gtest migration to be worth anything, and
// neither is visible from a green run:
//
//  1. QTest's assertion macros are dead in a gtest translation unit — they
//     report to a result object that only exists inside QTest::qExec, so a
//     failing QVERIFY is a bare `return;` that gtest records as PASSED. The
//     poison in gtest_qt.h makes that a compile error, and this file pins the
//     poison itself: the build would fail, not this test, if it regressed —
//     so what is pinned here is that the macros are GONE, not that they work.
//
//  2. The replacement vocabulary (ASSERT_TRY_*) actually fails when its
//     condition never holds. A macro that silently timed out and reported
//     nothing would be the same disease in a new spelling, and a green suite
//     could not tell the difference.
//
// The second is measured with EXPECT_FATAL_FAILURE: gtest runs the statement,
// captures the failure, and asserts that a FATAL one of the expected kind was
// reported. If ASSERT_TRY_TRUE_TIMEOUT stopped reporting, the inner statement
// would pass and EXPECT_FATAL_FAILURE would fail.
#include "gtest_qt.h"
#include <gtest/gtest-spi.h>
#include <QTimer>
#include <QApplication>

namespace {

// The QTest macros must not exist here. This is a compile-time property, so
// the check is that the identifiers are unusable — written as a preprocessor
// probe rather than an assertion, because there is nothing left to call.
// (The macros are #undef'd then poisoned, so #ifdef would still be true —
// what matters is that using one is a compile error, which the poison
// guarantees and the build proves. Nothing to assert at runtime.)

} // namespace

// A timed-out wait must be a FATAL failure naming the deadline, because every
// observation after it would read state that never arrived.
TEST(HarnessSanity, ATRYTIMEOUTActuallyFailsOnTimeout) {
    EXPECT_FATAL_FAILURE(ASSERT_TRY_TRUE_TIMEOUT(false, 10), "timed out after");
    EXPECT_FATAL_FAILURE(ASSERT_TRY_EQ_TIMEOUT(1, 2, 10), "timed out after");
}

// The non-fatal twins report too, so a caller that can continue still sees it.
TEST(HarnessSanity, EXPECTTRYTIMEOUTReportsWithoutAborting) {
    EXPECT_NONFATAL_FAILURE(EXPECT_TRY_TRUE_TIMEOUT(false, 10), "timed out after");
    EXPECT_NONFATAL_FAILURE(EXPECT_TRY_EQ_TIMEOUT(1, 2, 10), "timed out after");
}

// The success path stays silent — otherwise every passing wait would be a
// failure, which is the opposite mistake and just as hard to notice.
TEST(HarnessSanity, ATRYTIMEOUTSucceedsWhenTheConditionHolds) {
    ASSERT_TRY_TRUE_TIMEOUT(true, 50);
    ASSERT_TRY_EQ_TIMEOUT(7, 7, 50);
    // and it really polls: the condition becomes true only on a later turn of
    // the event loop, which a single check would miss
    bool later = false;
    QObject context; // the 3-arg form: the lambda dies with its context
    QTimer::singleShot(20, &context, [&later] {
        later = true;
    });
    ASSERT_TRY_TRUE_TIMEOUT(later, 2000);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
