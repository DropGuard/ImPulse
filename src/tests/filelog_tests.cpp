#include "gtest_qt.h"
#include "engine_stack_fixture.h"

// the file logging machinery in utils/filelog
class TestFileLog : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestFileLog() : EngineStackFixture("filelog") {
    }
};

// install() is thin glue verified by every session's first log lines; the
// pieces it composes are drivable here with tiny limits.
TEST_F(TestFileLog, fileLogFormatCarriesLevelCharAndTimestamp) {
    static const QRegularExpression shape("^\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d+ I hello\\n$");
    QString info = FileLog::formatLine(QtInfoMsg, "hello");
    if(!shape.match(info).hasMatch())
        qWarning() << "formatLine produced:" << info;
    EXPECT_TRUE(shape.match(info).hasMatch());
    EXPECT_EQ(FileLog::formatLine(QtDebugMsg, "x").section(' ', 2, 2), QStringLiteral("D"));
    EXPECT_EQ(FileLog::formatLine(QtWarningMsg, "x").section(' ', 2, 2), QStringLiteral("W"));
    EXPECT_EQ(FileLog::formatLine(QtCriticalMsg, "x").section(' ', 2, 2), QStringLiteral("C"));
    EXPECT_EQ(FileLog::formatLine(QtFatalMsg, "x").section(' ', 2, 2), QStringLiteral("F"));
}

TEST_F(TestFileLog, fileLogRotatesPartsAndStopsAtTheCap) {
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    // 31 bytes per line (30 'x' + '\n'); the banner is never counted, so
    // the rotation boundary is exactly where the handler's bytes land
    FileLog::SessionLog log(100, 3);
    EXPECT_TRUE(log.open(dir.path() + "/qimgv-test.log"));
    const QString line(30, 'x');
    for(int i = 0; i < 3; ++i)
        log.write(line);
    EXPECT_EQ(log.part(), 1);
    log.write(line); // 124 >= 100 -> part 2
    EXPECT_EQ(log.part(), 2);
    EXPECT_EQ(log.bytesInPart(), qint64(0));
    for(int i = 0; i < 4; ++i)
        log.write(line); // the 4th write rotates -> part 3
    EXPECT_EQ(log.part(), 3);
    for(int i = 0; i < 4; ++i)
        log.write(line); // at the cap: this write lands, then stop
    EXPECT_TRUE(!log.isWriting());
    EXPECT_EQ(QDir(dir.path()).entryList(QStringList() << "qimgv-test.log*", QDir::Files).size(), 3);
}

TEST_F(TestFileLog, fileLogPruneKeepsTheNewestTenSessions) {
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    for(int i = 1; i <= 12; ++i) {
        QFile f(dir.path() + QString("/qimgv-0101-%1.log").arg(i, 2, 10, QChar('0')));
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    }
    FileLog::pruneOldSessions(dir.path(), 10);
    QStringList left = QDir(dir.path()).entryList(QStringList() << "qimgv-*.log*", QDir::Files, QDir::Name);
    EXPECT_EQ(left.size(), 10);
    EXPECT_EQ(left.first(), QStringLiteral("qimgv-0101-03.log"));
    EXPECT_EQ(left.last(), QStringLiteral("qimgv-0101-12.log"));
}
