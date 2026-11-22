#include "gtest_qt.h"
#include <utility>
#include <QSignalSpy>
#include <QApplication>

#include "core_test_helpers.h"
#include "testhelpers.h"
#include "core.h"
#include "settings.h"
#include "sharedresources.h"
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/inputmap.h"
#include "gui/dialogs/settingsdialog.h"

class TestSlideshow : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons;
    QString m_base;

    void writeImage(const QString &path) {
        EXPECT_TRUE(makeImageFile(path, QSize(8, 8)));
    }
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(m_base).filePath(tag + QStringLiteral("-{}").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    // A headless Core over the three fakes, built on first use.
    Core &core() {
        if(!m_core)
            m_core = std::make_unique<Core>(headlessDeps());
        return *m_core;
    }

    Core::Deps headlessDeps() {
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &m_ui;
        deps.overlays = &m_overlays;
        deps.view = &m_view;
        return deps;
    }

    coretest::FakeInteractions m_ui;
    coretest::FakeOverlayHost m_overlays;
    coretest::RecordingView m_view;
    std::unique_ptr<Core> m_core;

    void SetUp() override {
        EXPECT_TRUE(m_home.path() != nullptr);
        m_base = QDir(m_home.path()).filePath("slideshow");
        EXPECT_TRUE(QDir().mkpath(m_base));
    }

    QStringList slideshowFiles(const QString &tag) {
        const QString dir = QDir(m_base).filePath(tag);
        // not an immediate return: this helper has a value to hand back — the
        // same reason dropPosOverThumbnail reports through a non-fatal failure
        if(!QDir().mkpath(dir))
            ADD_FAILURE_AT(__FILE__, __LINE__) << "cannot create the slideshow fixture directory";
        QStringList files;
        for(int i = 1; i <= 4; ++i) {
            const QString f = dir + QStringLiteral("/s%1.png").arg(i);
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            if(!img.save(f, "PNG"))
                ADD_FAILURE_AT(__FILE__, __LINE__) << qPrintable(QStringLiteral("cannot write ") + f);
            files << f;
        }
        return files;
    }

    // the QTRY_* vocabulary's gtest form: pump events until the condition
    // holds or the deadline expires. QTRY_VERIFY_WITH_TIMEOUT wraps QVERIFY,
    // whose failure path is a bare return that reports nothing outside a
    // QTest::qExec run — measured in this very conversion.
    static bool waitSettled(Core &c) {
        return QTest::qWaitFor(
            [&c] {
                return c.isSettled();
            },
            10000);
    }
};

TEST_F(TestSlideshow, slideshowLifecycleRequestsLoopPlaybackOffThenOn) {
    const QStringList files = slideshowFiles("show1");
    EXPECT_TRUE(core().loadPath(files.at(0)));
    EXPECT_TRUE(waitSettled(core()));
    // the loop request lands on the view seam; RecordingView keeps every value

    core().startSlideshow();
    EXPECT_TRUE(core().viewState().slideshow);
    EXPECT_EQ(m_view.loopPlaybackRequests, 1);
    EXPECT_EQ(m_view.loopPlaybackValues.at(0), false);

    // a second start with the flag already set is the no-op it must be:
    // re-arming would emit a second request and re-enter enableDocumentView
    core().startSlideshow();
    EXPECT_EQ(m_view.loopPlaybackRequests, 1);

    core().stopSlideshow();
    EXPECT_TRUE(!core().viewState().slideshow);
    EXPECT_EQ(m_view.loopPlaybackRequests, 2);
    EXPECT_EQ(m_view.loopPlaybackValues.at(1), true);

    core().stopSlideshow();
    EXPECT_EQ(m_view.loopPlaybackRequests, 2);
}

TEST_F(TestSlideshow, slideshowAdvanceIsSequentialAndArmsTheTimer) {
    const QStringList files = slideshowFiles("show2");
    EXPECT_TRUE(core().loadPath(files.at(0)));
    EXPECT_TRUE(waitSettled(core()));

    core().startSlideshow();
    // startSlideshow arms the timer itself, through startSlideshowTimer,
    // for a static image — measured, this assertion's first draft claimed
    // the opposite and failed
    EXPECT_TRUE(core().slideshowClock().isActive());

    core().nextImageSlideshow();
    EXPECT_EQ(core().currentFilePath(), files.at(1));
    EXPECT_TRUE(core().slideshowClock().isActive()) << "a static image under slideshow must keep the timer armed";

    core().nextImageSlideshow();
    EXPECT_EQ(core().currentFilePath(), files.at(2));
}

TEST_F(TestSlideshow, nonLoopingSlideshowStopsAtTheEndOfTheListing) {
    const QStringList files = slideshowFiles("show3");
    EXPECT_TRUE(core().loadPath(files.at(0)));
    EXPECT_TRUE(waitSettled(core()));
    EXPECT_TRUE(!core().slideshowLoops());

    core().jumpToLast();
    EXPECT_EQ(core().currentFilePath(), files.at(3));
    // jumpToLast stops the show, so the state below is a fresh arm
    EXPECT_TRUE(!core().viewState().slideshow);

    core().jumpToFirst();
    EXPECT_EQ(core().currentFilePath(), files.at(0));

    core().startSlideshow();
    core().jumpToLast();
    core().startSlideshow();
    EXPECT_TRUE(core().viewState().slideshow);
    const QString atEnd = core().currentFilePath();

    core().nextImageSlideshow();
    EXPECT_EQ(core().currentFilePath(), atEnd);
    EXPECT_TRUE(!core().viewState().slideshow)
        << "a non-looping slideshow must stop itself at the end of the directory";
    EXPECT_TRUE(!core().slideshowClock().isActive());
}

TEST_F(TestSlideshow, shuffleKeepsTheSlideshowCyclingPastTheEnd) {
    const QStringList files = slideshowFiles("show4");
    EXPECT_TRUE(core().loadPath(files.at(0)));
    EXPECT_TRUE(waitSettled(core()));
    EXPECT_TRUE(!core().viewState().shuffle);

    core().toggleShuffle();
    EXPECT_TRUE(core().viewState().shuffle); // this is what syncRandomizer sized for

    core().startSlideshow();
    core().jumpToLast();
    core().startSlideshow();
    EXPECT_TRUE(core().viewState().slideshow);

    // the sequential branch would have stopped here (pinned above with the
    // same setup); the randomizer branch starts a new cycle instead
    core().nextImageSlideshow();
    EXPECT_TRUE(core().viewState().slideshow) << "shuffle mode stopped at the end of the listing";
    EXPECT_TRUE(files.contains(core().currentFilePath()));
}

TEST_F(TestSlideshow, toggleSlideshowStartsThenStops) {
    const QStringList files = slideshowFiles("show5");
    EXPECT_TRUE(core().loadPath(files.at(0)));
    EXPECT_TRUE(waitSettled(core()));
    EXPECT_TRUE(!core().viewState().slideshow);

    core().toggleSlideshow();
    EXPECT_TRUE(core().viewState().slideshow);
    EXPECT_TRUE(core().slideshowClock().isActive());

    core().toggleSlideshow();
    EXPECT_TRUE(!core().viewState().slideshow) << "the second toggle did not stop the slideshow";
    EXPECT_TRUE(!core().slideshowClock().isActive());
}

// The slideshow and editing are mutually exclusive states. startSlideshow
// already refuses to start over an uncommitted edit (navigationDenied's
// state leg); this is the closing half — a running clock refuses the edit,
// because the next tick's unloadExcept would discard it. The refusal
// speaks: the clock's owner is the party that would destroy the work.
TEST_F(TestSlideshow, editingIsRefusedWhileTheSlideshowRuns) {
    const QStringList files = slideshowFiles("show6");
    EXPECT_TRUE(core().loadPath(files.at(0)));
    EXPECT_TRUE(waitSettled(core()));
    auto img = core().directoryModel()->getImage(core().currentFilePath());
    ASSERT_TRUE(img); // every observation below dereferences the image
    EXPECT_TRUE(!img->isEdited());

    core().startSlideshow();
    EXPECT_TRUE(core().viewState().slideshow);
    const int messagesBefore = m_overlays.messageCalls;

    core().flipH();
    EXPECT_TRUE(!img->isEdited()) << "the edit landed while the slideshow was running";
    EXPECT_EQ(m_overlays.messageCalls, messagesBefore + 1);
    EXPECT_TRUE(m_overlays.lastMessage.contains(QStringLiteral("Stop the slideshow")))
        << qPrintable(QStringLiteral("unexpected refusal text: ") + m_overlays.lastMessage);
    EXPECT_TRUE(core().viewState().slideshow);        // the clock is untouched
    EXPECT_EQ(core().currentFilePath(), files.at(0)); // and nothing moved

    // the refusal is the state's, not flipH's: another edit verb goes
    // through the same gate
    core().rotateRight();
    EXPECT_EQ(m_overlays.messageCalls, messagesBefore + 2);
    EXPECT_TRUE(!img->isEdited());

    // stopping the slideshow is the whole resolution — nothing else
    core().stopSlideshow();
    core().flipH();
    // Re-read: an edit is a draft the model holds against the path, and
    // getImage() answers with it. The pointer captured at the top is the
    // ORIGINAL, which an edit does not mutate in place.
    const auto afterEdit = core().directoryModel()->getImage(core().currentFilePath());
    ASSERT_TRUE(afterEdit);
    EXPECT_TRUE(afterEdit->isEdited()) << "the edit stayed refused after the clock stopped";
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
