#include "engine_stack_fixture.h"

#include <functional>
#include <utility> // std::as_const: a range loop over a non-const QList may detach

// the declarative settings table: clamps, junk values, and the config isolation
// the binding table stands on
class TestSettingsTable : public QObject, protected EngineStackFixture {
    Q_OBJECT
  public:
    TestSettingsTable() : EngineStackFixture("settings") {
    }

  private slots:
    void initTestCase() {
        init();
    }

    // Every row of settings_table.h is walked, so a new setting inherits this
    // coverage the moment it is added to the table: missing key -> default,
    // junk value -> default (not QVariant's silent zero/false), clamp bounds
    // hold on both ends.

    void settingsTableRowsSurviveJunkAndClamps() {
        FreshSettings fresh;
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());

        // missing key: every row serves its default
        for(const SettingDesc *d : kAllSettings) {
            switch(d->kind) {
            case SettingDesc::Kind::Bool: QCOMPARE(settings->load<bool>(*d), d->defB); break;
            case SettingDesc::Kind::Int: QCOMPARE(settings->load<qlonglong>(*d), d->defI); break;
            case SettingDesc::Kind::Real: QCOMPARE(settings->load<double>(*d), d->defR); break;
            case SettingDesc::Kind::Str: QCOMPARE(settings->load<QString>(*d), QString::fromUtf8(d->defS)); break;
            }
        }

        // junk string for every row: numbers fall back to the default instead
        // of QVariant's silent zero/false
        for(const SettingDesc *d : kAllSettings)
            junk.setValue(d->key, QVariant(QStringLiteral("banana")));
        junk.sync();
        for(const SettingDesc *d : kAllSettings) {
            switch(d->kind) {
            case SettingDesc::Kind::Bool:
                settings->load<bool>(*d); // defined, do not pin QVariant's whim
                break;
            case SettingDesc::Kind::Int: QCOMPARE(settings->load<qlonglong>(*d), d->defI); break;
            case SettingDesc::Kind::Real: QCOMPARE(settings->load<double>(*d), d->defR); break;
            case SettingDesc::Kind::Str: QCOMPARE(settings->load<QString>(*d), QStringLiteral("banana")); break;
            }
        }

        // Every row that declares bounds is fed a value outside them, and the
        // expectation comes from the row's own policy: clamp moves it to the
        // nearest bound, reset discards it for the default. Two policies
        // because two real needs exist - an out-of-range enum member is a
        // broken value (want the default), an out-of-range pixel size is a
        // real size that merely overshot (want the bound).
        // This walks load(), so it covers the machinery: it verifies each
        // declared policy is implemented, not that an accessor uses it. The
        // accessor-level pins are the other two tests below.
        QStringList policyMismatch;
        for(const SettingDesc *d : kAllSettings) {
            if(!d->clamped())
                continue;
            QList<double> outside; // only the declared side
            if(d->hasLo)
                outside << d->lo - 1000.0;
            if(d->hasHi)
                outside << d->hi + 1000.0;
            for(const double bad : std::as_const(outside)) {
                if(d->kind == SettingDesc::Kind::Int) {
                    junk.setValue(d->key, QVariant(static_cast<qlonglong>(bad)));
                    junk.sync();
                    const qlonglong want =
                        d->resetOnRange ? d->defI
                                        : qBound<qlonglong>(static_cast<qlonglong>(d->lo), static_cast<qlonglong>(bad),
                                                            static_cast<qlonglong>(d->hi));
                    const qlonglong got = settings->load<qlonglong>(*d);
                    if(got != want)
                        policyMismatch << QStringLiteral("%1 stored=%2 got=%3 want=%4 (%5)")
                                              .arg(QString::fromUtf8(d->key))
                                              .arg(bad)
                                              .arg(got)
                                              .arg(want)
                                              .arg(d->resetOnRange ? QStringLiteral("reset") : QStringLiteral("clamp"));
                } else if(d->kind == SettingDesc::Kind::Real) {
                    junk.setValue(d->key, QVariant(bad));
                    junk.sync();
                    const double want = d->resetOnRange ? d->defR : qBound(d->lo, bad, d->hi);
                    const double got = settings->load<double>(*d);
                    if(qAbs(got - want) > 1e-9)
                        policyMismatch << QStringLiteral("%1 stored=%2 got=%3 want=%4 (%5)")
                                              .arg(QString::fromUtf8(d->key))
                                              .arg(bad)
                                              .arg(got)
                                              .arg(want)
                                              .arg(d->resetOnRange ? QStringLiteral("reset") : QStringLiteral("clamp"));
                }
            }
        }
        QVERIFY2(policyMismatch.isEmpty(),
                 qPrintable(QStringLiteral("rows did not apply their declared range policy:\n  ") +
                            policyMismatch.join(QStringLiteral("\n  "))));
    }

    // QSettings' second argument only covers a MISSING key: a junk value
    // converts to 0/false, and if 0 happens to be legal the hand-rolled guard
    // never fires. That is how JPEGSaveQuality ended up saving at quality 0
    // from a corrupt config while its row declared the default 95.
    void jpegSaveQualityServesTheDeclaredDefaultOnJunk() {
        FreshSettings fresh;
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());

        junk.setValue("JPEGSaveQuality", QVariant(QStringLiteral("banana")));
        junk.sync();
        QCOMPARE(settings->JPEGSaveQuality(), 95);

        junk.setValue("JPEGSaveQuality", QVariant(QString()));
        junk.sync();
        QCOMPARE(settings->JPEGSaveQuality(), 95);

        junk.setValue("JPEGSaveQuality", QVariant(static_cast<qlonglong>(140)));
        junk.sync();
        QCOMPARE(settings->JPEGSaveQuality(), 100); // the row's hi bound
    }

    // The accessors that used to read QSettings by hand and disagreed with
    // their row on what a junk value means. Collected then asserted together:
    // QCOMPARE returns from the test function at the first mismatch, which
    // would otherwise hide every case after the first.
    void junkFallsBackToTheDeclaredDefaultNotToSomeOtherValue() {
        FreshSettings fresh;
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());
        const QList<QByteArray> keys{"backgroundOpacity",     "folderViewMode", "thumbPanelStyle",
                                     "memoryAllocationLimit", "imageScrolling", "focusPointIn1to1Mode"};
        for(const QByteArray &k : keys)
            junk.setValue(QString::fromUtf8(k), QVariant(QStringLiteral("banana")));
        junk.sync();

        QStringList got{QStringLiteral("backgroundOpacity=%1").arg(settings->backgroundOpacity()),
                        QStringLiteral("folderViewMode=%1").arg(settings->folderViewMode()),
                        QStringLiteral("thumbPanelStyle=%1").arg(settings->thumbPanelStyle()),
                        QStringLiteral("memoryAllocationLimit=%1").arg(settings->memoryAllocationLimit()),
                        QStringLiteral("imageScrolling=%1").arg(settings->imageScrolling()),
                        QStringLiteral("focusPointIn1to1Mode=%1").arg(settings->focusPointIn1to1Mode())};
        const QStringList want{QStringLiteral("backgroundOpacity=1"), QStringLiteral("folderViewMode=2"),
                               QStringLiteral("thumbPanelStyle=1"),   QStringLiteral("memoryAllocationLimit=1024"),
                               QStringLiteral("imageScrolling=1"),    QStringLiteral("focusPointIn1to1Mode=1")};
        QStringList wrong;
        for(int i = 0; i < got.size(); ++i)
            if(got.at(i) != want.at(i))
                wrong << QStringLiteral("%1 (row wants %2)").arg(got.at(i), want.at(i).section('=', 1));
        QVERIFY2(wrong.isEmpty(), qPrintable(QStringLiteral("junk values did not land on the declared defaults: ") +
                                             wrong.join(QStringLiteral("; "))));
    }

    // The reset-policy rows, pinned through their production accessors against
    // the default each row declares. Deliberately not a comparison with load():
    // expecting whatever load() answers is self-consistent and inert - flipping
    // a row's policy moves that expectation with it, and the test stays green
    // while every enum setting changes meaning. Measured: declaring
    // K_ZoomIndicatorMode with the clamp policy fails this test by name while
    // the machinery audit above does not.
    void outOfRangeSettingsResolveThroughTheirAccessors() {
        FreshSettings fresh;
        QSettings probe(QCoreApplication::organizationName(), QCoreApplication::applicationName());

        struct Item {
            const SettingDesc *row;
            std::function<double()> get;
        };
        const QList<Item> items{
            {&K_SortingMode,
             [] {
                 return double(settings->sortingMode());
             }},
            {&K_FolderViewMode,
             [] {
                 return double(settings->folderViewMode());
             }},
            {&K_ThumbPanelStyle,
             [] {
                 return double(settings->thumbPanelStyle());
             }},
            {&K_DefaultFitMode,
             [] {
                 return double(settings->imageFitMode());
             }},
            {&K_ZoomIndicatorMode,
             [] {
                 return double(settings->zoomIndicatorMode());
             }},
            {&K_FocusPointIn1to1Mode,
             [] {
                 return double(settings->focusPointIn1to1Mode());
             }},
            {&K_ImageScrolling,
             [] {
                 return double(settings->imageScrolling());
             }},
            {&K_DefaultCropAction,
             [] {
                 return double(settings->defaultCropAction());
             }},
            {&K_DefaultViewMode,
             [] {
                 return double(settings->defaultViewMode());
             }},
            {&K_FolderEndAction,
             [] {
                 return double(settings->folderEndAction());
             }},
            {&K_AutoResizeLimit,
             [] {
                 return double(settings->autoResizeLimit());
             }},
            {&K_SlideshowInterval,
             [] {
                 return double(settings->slideshowInterval());
             }},
            {&K_ThumbnailerThreads,
             [] {
                 return double(settings->thumbnailerThreadCount());
             }},
        };

        QStringList wrong;
        for(const Item &it : items) {
            const SettingDesc *d = it.row;
            QList<qlonglong> outside; // only the declared side:
            if(d->hasLo)
                outside << static_cast<qlonglong>(d->lo) - 1000;
            if(d->hasHi)
                outside << static_cast<qlonglong>(d->hi) + 1000;
            // a row without hi has no upper violation to probe
            for(const qlonglong bad : std::as_const(outside)) {
                probe.setValue(d->key, QVariant(bad));
                probe.sync();
                if(it.get() != static_cast<double>(d->defI))
                    wrong << QStringLiteral("%1 stored=%2 accessor gave %3, row default is %4")
                                 .arg(QString::fromUtf8(d->key))
                                 .arg(bad)
                                 .arg(it.get())
                                 .arg(d->defI);
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(QStringLiteral("reset-policy settings did not serve their default:\n  ") +
                                             wrong.join(QStringLiteral("\n  "))));
    }

    void settingsEnumGettersClampJunkConfigValues() {
        // a hand-edited config file can hold anything; every enum getter must
        // land on a valid member instead of handing garbage to a switch
        settings->setSortingMode(static_cast<SortingMode>(42));
        QVERIFY(settings->sortingMode() == SORT_NAME);
        settings->setSortingMode(static_cast<SortingMode>(-1));
        QVERIFY(settings->sortingMode() == SORT_NAME);
        settings->setSortingMode(SORT_NAME);

        settings->setPanelPosition(static_cast<PanelPosition>(42));
        QVERIFY(settings->panelPosition() == PANEL_RIGHT); // documented fallback
        settings->setPanelPosition(PANEL_TOP);

        // folderEndAction clamping is pinned in its own test; this is the
        // same trap one layer down
        settings->setFolderEndAction(static_cast<FolderEndAction>(99));
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
    }

    // the state-side list: saved paths round-trip through the state config
    // (IsolatedHome keeps this off the developer's real state)
    void savedPathsRoundTrips() {
        FreshSettings fresh;
        settings->setSavedPaths({"/home/user/pictures"});
        QCOMPARE(settings->savedPaths(), QStringList{"/home/user/pictures"});
    }

    // the open-dialog filter policy: Qt's decodable mime types plus the
    // hardcoded video formats, gated by videoPlayback
    void supportedMimeTypesFollowVideoPlaybackPolicy() {
        FreshSettings fresh;
        settings->setVideoPlayback(true);
        QVERIFY(settings->supportedMimeTypes().contains("video/mp4"));
        QVERIFY(settings->supportedMimeTypes().contains("image/jpeg"));
        settings->setVideoPlayback(false);
        QVERIFY(!settings->supportedMimeTypes().contains("video/mp4"));
        QVERIFY(settings->supportedMimeTypes().contains("image/png"));
    }

    // the shortcut map's round trip through the ini, including the "eq" escape
    // for names that would collide with the k=v encoding
    void shortcutRoundTripEscapesTrailingEquals() {
        FreshSettings fresh;
        QMap<QString, QString> shortcuts;
        shortcuts.insert("ctrl+x", "25");
        shortcuts.insert("ctrl+=", "26"); // the "=" key needs the escape
        settings->saveShortcuts(shortcuts);
        QMap<QString, QString> back;
        settings->readShortcuts(back);
        QCOMPARE(back.value("ctrl+x"), QStringLiteral("25"));
        QCOMPARE(back.value("ctrl+="), QStringLiteral("26"));
    }

    void freshSettingsGuardIsolatesConfigState() {
        // without the guard, every test had to reset the keys it depends on
        // and the suite was silently order-sensitive
        settings->setSortingMode(SORT_TIME);
        QVERIFY(settings->sortingMode() == SORT_TIME);
        {
            FreshSettings fresh; // swaps the global to a defaults-only instance
            QVERIFY(settings->sortingMode() == SORT_NAME);
            settings->setSortingMode(SORT_SIZE_DESC);
            QVERIFY(settings->sortingMode() == SORT_SIZE_DESC);
        }
        // the original instance and its in-memory state are back
        QVERIFY(settings->sortingMode() == SORT_TIME);
    }

    // Qt6 killed the @Variant channel for custom types in QSettings: the old
    // Script storage wrote empty shells and read nothing back, silently
    // destroying scripts on every settings save. Scripts now persist as JSON
    // in the same value slot, with a fallback to the pre-@Variant plain keys.
    void scriptsRoundTripAsJsonNotMetatypeShells() {
        FreshSettings fresh;
        QMap<QString, Script> in;
        in.insert(QStringLiteral("Favorite"), Script(QStringLiteral("/usr/local/bin/fav a b %file%"), true));
        settings->saveScripts(in);
        QMap<QString, Script> out;
        settings->readScripts(out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.value("Favorite").command, QStringLiteral("/usr/local/bin/fav a b %file%"));
        QCOMPARE(out.value("Favorite").blocking, true);
        // the stored form must be a JSON string, not a @Variant(...) shell
        QSettings probe(QCoreApplication::organizationName(), QCoreApplication::applicationName());
        probe.beginGroup("Scripts");
        const QVariant raw = probe.value("script/1/value");
        probe.endGroup();
        QCOMPARE(raw.typeId(), QMetaType::QString);
        QVERIFY(raw.toString().startsWith('{'));
    }

    void scriptsFallBackToPreJsonPlainKeys() {
        FreshSettings fresh;
        QSettings conf(QCoreApplication::organizationName(), QCoreApplication::applicationName());
        conf.beginGroup("Scripts");
        conf.beginWriteArray("script");
        conf.setArrayIndex(0);
        conf.setValue("name", QStringLiteral("Favorite"));
        conf.setValue("value", QStringLiteral("@Variant(\\0\\0\\0\\x7f\\0\\0\\0\\aScript\\0)"));
        conf.endArray();
        conf.setValue("1/command", QStringLiteral("/usr/local/bin/fav %file%"));
        conf.endGroup();
        conf.sync();
        QMap<QString, Script> out;
        settings->readScripts(out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.value("Favorite").command, QStringLiteral("/usr/local/bin/fav %file%"));
    }

    void resolveCacheDirHonorsExplicitCacheDirKey() {
        // main() installs the file log with this resolver before Settings
        // exists; a configured cacheDir must be honored there exactly as it
        // is inside setupCache, or logs and thumbnails split across dirs
        FreshSettings fresh;
        const QString alt =
            QDir::tempPath() + QStringLiteral("/qimgv-cache-alt-%1").arg(QCoreApplication::applicationPid());
        QSettings conf;
        conf.setValue(QStringLiteral("cacheDir"), alt);
        conf.sync();
        QCOMPARE(Settings::resolveCacheDir(), alt);
        QVERIFY(QDir(alt).exists());
    }

    // Importing a qimgv configuration is an explicit request, not a startup
    // migration: until 2026.11 this fork and upstream qimgv shared the identity
    // "qimgv/qimgv", so the directory is equally likely to be an upstream
    // installation whose files are not ours to read unasked.
    //
    // Pinned here are the decisions this function makes and could get wrong:
    // which file becomes which, that an existing destination is never
    // overwritten, and that "the source has nothing" is its own answer rather
    // than a failure. NOT pinned: that QFile::copy leaves its source alone, and
    // that a function which is never handed a cache path does not touch one —
    // both are properties of Qt and of the signature, and a test that restates
    // them cannot fail for a reason this code owns.
    void importingAQimgvConfigurationCopiesAndNeverOverwrites() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString source = scratch.filePath("qimgv-conf");
        const QString dest = scratch.filePath("impulse-conf");
        QDir().mkpath(source);
        writeTestFile(source + "/qimgv.conf", "theConf=imported\n");
        writeTestFile(source + "/theme.conf", "[theme]\n");

        auto report = Settings::importQimgvConfig(source, dest);
        QVERIFY(report.outcome == Settings::ImportOutcome::Imported);
        QVERIFY(report.detail.isEmpty());
        // qimgv.conf is RENAMED on the way in; the companions keep their names
        QCOMPARE(report.copied.size(), 2);
        QCOMPARE(readTestFile(dest + "/impulse.conf"), QByteArray("theConf=imported\n"));
        QVERIFY(QFile::exists(dest + "/theme.conf"));
        // a file qimgv never had is not invented
        QVERIFY(!QFile::exists(dest + "/savedState.conf"));

        // a second run does not clobber what this app has been running with —
        // this is the decision, not a side effect: the alternative (prefer the
        // source) would silently revert the user's current settings
        writeTestFile(dest + "/impulse.conf", "theConf=current\n");
        writeTestFile(source + "/qimgv.conf", "theConf=upstream-edited-since\n");
        auto again = Settings::importQimgvConfig(source, dest);
        QVERIFY(again.outcome == Settings::ImportOutcome::AllSkipped);
        QVERIFY(again.skipped.contains(QStringLiteral("impulse.conf")));
        QCOMPARE(readTestFile(dest + "/impulse.conf"), QByteArray("theConf=current\n"));

        // no qimgv.conf at all: "nothing to import", a distinct answer from a
        // failure, because the dialog says something different for each
        QTemporaryDir empty;
        QVERIFY(empty.isValid());
        auto none = Settings::importQimgvConfig(empty.path(), scratch.filePath("other-dest"));
        QVERIFY(none.outcome == Settings::ImportOutcome::NoSource);
        QVERIFY(none.copied.isEmpty());
    }

    // The import reads the directory where QSettings actually puts the legacy
    // identity. Pinned against QSettings itself, not against a re-spelling of
    // the same expression: a wrong guess makes the feature silently find
    // nothing on every machine, and comparing the implementation to itself
    // cannot catch that.
    void theImportSourceIsWhereQSettingsPutsTheLegacyIdentity() {
        const QString beforeOrg = QCoreApplication::organizationName();
        const QString beforeApp = QCoreApplication::applicationName();
        // the shared legacy identity, exactly as the 2026.11 rename left it
        QCoreApplication::setOrganizationName(QStringLiteral("qimgv"));
        QCoreApplication::setApplicationName(QStringLiteral("qimgv"));
        const QString fromQSettings = QFileInfo(QSettings().fileName()).absolutePath();
        QCoreApplication::setOrganizationName(beforeOrg);
        QCoreApplication::setApplicationName(beforeApp);

        QCOMPARE(Settings::qimgvConfigDir(), fromQSettings);
    }

    // This app's pre-XDG private store is removed once per cache root.
    // Four things have to hold, and each has its own way of going wrong: our
    // store goes, the upstream "qimgv" store SURVIVES (upstream may still be
    // installed, and it is not ours to make regenerate), unrelated things in the
    // cache root stay, and the marker makes the second call a no-op even when
    // the directory has come back.
    void retirePrivateThumbnailStoresRemovesOnlyOurOwnStore() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString base = root.path();
        QDir().mkpath(base + "/impulse/thumbnails");
        QDir().mkpath(base + "/qimgv/thumbnails"); // upstream's, by name
        QDir().mkpath(base + "/impulse/logs");
        QDir().mkpath(base + "/other-app/thumbnails");
        QDir().mkpath(base + "/thumbnails"); // the shared store itself
        writeTestFile(base + "/impulse/thumbnails/a.png", "png");
        writeTestFile(base + "/qimgv/thumbnails/b.png", "png");
        writeTestFile(base + "/impulse/logs/session.log", "log");
        writeTestFile(base + "/other-app/thumbnails/c.png", "png");
        writeTestFile(base + "/thumbnails/d.png", "png");

        QCoreApplication::setApplicationName(QStringLiteral("impulse"));
        Settings::retirePrivateThumbnailStores(base);

        QVERIFY(!QDir(base + "/impulse/thumbnails").exists());
        QVERIFY2(QDir(base + "/qimgv/thumbnails").exists(),
                 "the legacy name belongs to upstream too — deleting it would make upstream regenerate everything");
        QVERIFY2(QDir(base + "/impulse/logs").exists(), "the app's own scratch space is not a thumbnail store");
        QVERIFY2(QDir(base + "/other-app/thumbnails").exists(), "another app's cache is not ours to delete");
        QVERIFY2(QDir(base + "/thumbnails").exists(), "the shared store is the replacement, not a casualty");

        // the marker: a store that reappears (an older binary ran again) is not
        // deleted on the next start — the migration already happened
        QDir().mkpath(base + "/impulse/thumbnails");
        writeTestFile(base + "/impulse/thumbnails/again.png", "png");
        Settings::retirePrivateThumbnailStores(base);
        QVERIFY2(QFile::exists(base + "/impulse/thumbnails/again.png"), "the retirement runs once, not on every start");
    }
};

#include "settings_tests.moc"

int runSettingsTableTests(int argc, char **argv) {
    TestSettingsTable test;
    return runIfOwned(test, argc, argv);
}
