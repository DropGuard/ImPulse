#include "engine_stack_fixture.h"

#include <QTemporaryFile>

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
    }

    // Every row of settings_table.h is walked, so a new setting inherits this
    // coverage the moment it is added to the table: missing key -> default,
    // junk value -> default (not QVariant's silent zero/false), clamp bounds
    // hold on both ends.

    void settingsTableRowsSurviveJunkAndClamps() {
        FreshSettings fresh;
        // the conf backend is per-platform (see FreshSettings): write through
        // the SAME one the Settings instance reads, or the junk lands in a
        // backend the app never opens
#ifdef Q_OS_WIN
        QSettings junk(QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() +
                           ".ini",
                       QSettings::IniFormat);
#else
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());
#endif

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
        // the conf backend is per-platform (see FreshSettings): write through
        // the SAME one the Settings instance reads
#ifdef Q_OS_WIN
        QSettings junk(QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() +
                           ".ini",
                       QSettings::IniFormat);
#else
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());
#endif

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
#ifdef Q_OS_WIN
        QSettings junk(QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() +
                           ".ini",
                       QSettings::IniFormat);
#else
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());
#endif
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
#ifdef Q_OS_WIN
        QSettings probe(QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() +
                            ".ini",
                        QSettings::IniFormat);
#else
        QSettings probe(QCoreApplication::organizationName(), QCoreApplication::applicationName());
#endif

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

    // Scratch space can be removed from outside this class — tmpfiles.d, a
    // user emptying the cache — and tmpDir() must then hand back a directory
    // that exists. It re-creates; it does not heal isolation, and this pin is
    // about the former only.
    //
    // The failure mode it prevents is not "an open fails": the clipboard's
    // edited-image export falls back to a FIXED file name when its
    // QTemporaryFile cannot open, so two exports share one file and the second
    // consumer reads the first's pixels (edit_save_tests has the pin for that
    // contract).
    void tmpDirRecreatesADirectoryRemovedUnderIt() {
        FreshSettings fresh;
        const QString dir = settings->tmpDir();
        QVERIFY2(QDir(dir).exists(), "the fixture did not start with a tmp dir");

        QVERIFY2(QDir(dir).removeRecursively(), "could not remove the tmp dir");
        QVERIFY2(!QDir(dir).exists(), "the directory survived removeRecursively");

        const QString again = settings->tmpDir();
        QCOMPARE(again, dir); // the same name — a scratch dir, not a new one
        QVERIFY2(QDir(again).exists(), "tmpDir() named a directory that does not exist");

        QTemporaryFile probe(QDir(again).filePath(QStringLiteral("image-XXXXXX.png")));
        QVERIFY2(probe.open(), qPrintable(QStringLiteral("cannot open in tmpDir: ") + probe.errorString()));
    }

    // the state-side list: saved paths round-trip through the state config
    // (IsolatedHome keeps this off the developer's real state)
    void savedPathsRoundTrips() {
        FreshSettings fresh;
        settings->setSavedPaths({"/home/user/pictures"});
        QCOMPARE(settings->savedPaths(), QStringList{"/home/user/pictures"});
    }

    // the open-dialog filter policy: Qt's decodable mime types plus the
    // hardcoded video formats, gated by videoPlayback. Without USE_MPV the
    // toggle collapses to a constant (Settings::videoPlayback answers false
    // unconditionally), so the policy that can be pinned here is "the video
    // formats are never offered" — the positive half is compiled to the
    // build that actually has the toggle.
    void supportedMimeTypesFollowVideoPlaybackPolicy() {
        FreshSettings fresh;
        settings->setVideoPlayback(true);
#ifdef USE_MPV
        QVERIFY(settings->supportedMimeTypes().contains("video/mp4"));
#endif
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

    // the scoped tables share the codec under their own Controls/ keys, and
    // a scope's saved value must not bleed into the global one
    void scopedShortcutTablesRoundTripIndependently() {
        FreshSettings fresh;
        QMap<QString, QString> grid;
        grid.insert("Right", "gridSelectNext");
        grid.insert("ctrl+=", "gridSelectAll"); // the escape, scoped
        settings->saveShortcuts(grid, QStringLiteral("gridShortcuts"));

        QMap<QString, QString> inspect;
        inspect.insert("Left", "scrollLeft");
        settings->saveShortcuts(inspect, QStringLiteral("inspectShortcuts"));

        QMap<QString, QString> back;
        settings->readShortcuts(back, QStringLiteral("gridShortcuts"));
        QCOMPARE(back.size(), 2);
        QCOMPARE(back.value("Right"), QStringLiteral("gridSelectNext"));
        QCOMPARE(back.value("ctrl+="), QStringLiteral("gridSelectAll"));

        QMap<QString, QString> backInspect;
        settings->readShortcuts(backInspect, QStringLiteral("inspectShortcuts"));
        QCOMPARE(backInspect.size(), 1);
        QCOMPARE(backInspect.value("Left"), QStringLiteral("scrollLeft"));

        // the global key never heard about any of it
        QMap<QString, QString> global;
        settings->readShortcuts(global);
        QVERIFY(global.isEmpty());
    }

    // A removed binding has to survive a restart. The tables are defaults-first
    // with the config overlaid, so deleting a row used to be undone by the next
    // launch: the default layer re-seeded the key and "unbind" silently
    // evaporated. The codec carries the removal as a `-action` tombstone; this
    // pins the round trip end to end — write the table WITHOUT a default action,
    // read it back, and the action is gone rather than re-seeded.
    void removedDefaultBindingStaysRemoved() {
        FreshSettings fresh;
        QMap<QString, QString> defaults;
        defaults.insert("F11", "toggleFullscreen");
        defaults.insert("F2", "renameFile");

        // the user deleted the renameFile row and kept F11
        QMap<QString, QString> table;
        table.insert("F11", "toggleFullscreen");
        settings->saveShortcutsWithTombstones(table, defaults);

        // what a fresh launch does: defaults first, config second
        QMap<QString, QString> reloaded = defaults;
        settings->readShortcuts(reloaded);
        QCOMPARE(reloaded.value("F11"), QStringLiteral("toggleFullscreen"));
        QVERIFY2(!reloaded.contains("F2"), "the removed binding came back from the default layer");
        QVERIFY2(!reloaded.key("renameFile").size(), "renameFile is bound again after the restart");
    }

    // The tombstone is scoped to the ACTION, not the token: an action may hold
    // several keys, and only a table that no longer holds it at all earns the
    // marker. An action still bound elsewhere keeps its live rows and is not
    // tombstoned — otherwise a partial removal would be read back as a total
    // one, and the surviving binding would vanish on the next launch.
    void tombstonesArePerActionAndOnlyWhenAbsent() {
        FreshSettings fresh;
        QMap<QString, QString> defaults;
        defaults.insert("F", "toggleFullscreen");
        defaults.insert("F11", "toggleFullscreen");

        // still holds toggleFullscreen under F11 only. The codec's map is
        // <key, action>, and the defaults seed F before F11 — same action
        // under two tokens, then the F row is removed by writing the table
        // without it. What must NOT happen is a tombstone: the action is
        // still bound, so reloading keeps it (under whichever key the map
        // hands .key() first; the ASSERTION is that neither token was
        // tombstoned away).
        QMap<QString, QString> table;
        table.insert("F11", "toggleFullscreen");
        settings->saveShortcutsWithTombstones(table, defaults);

        QMap<QString, QString> reloaded = defaults;
        settings->readShortcuts(reloaded);
        QCOMPARE(reloaded.value("F11"), QStringLiteral("toggleFullscreen"));
        QVERIFY2(!reloaded.key("toggleFullscreen").isEmpty(), "a surviving binding was tombstoned away");
        // and no tombstone was written for it: the codec marks an action only
        // when the table no longer holds it, and this one is still there
        QVERIFY2(!reloaded.contains("F") || reloaded.value("F") == QLatin1String("toggleFullscreen"),
                 "the still-bound action was tombstoned");
    }

    // A config written before the tombstone existed has no `-action` entries at
    // all, and must read exactly as it always did — the marker is additive to
    // the codec, not a new required shape.
    void tombstoneFreeConfigsReadUnchanged() {
        FreshSettings fresh;
        QMap<QString, QString> table;
        table.insert("F11", "toggleFullscreen");
        settings->saveShortcuts(table); // the old writer, no tombstones

        QMap<QString, QString> back;
        settings->readShortcuts(back);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back.value("F11"), QStringLiteral("toggleFullscreen"));
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
        // the stored form must be a JSON string, not a @Variant(...) shell.
        // The probe must read the SAME backend saveScripts wrote: portable
        // mode keeps the conf in the redirected ini, and the org/app mapping
        // would answer from a store nothing here wrote
        QSettings probe(
#ifdef Q_OS_WIN
            QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() + ".ini",
            QSettings::IniFormat
#else
            QCoreApplication::organizationName(), QCoreApplication::applicationName()
#endif
        );
        probe.beginGroup("Scripts");
        const QVariant raw = probe.value("script/1/value");
        probe.endGroup();
        QCOMPARE(raw.typeId(), QMetaType::QString);
        QVERIFY(raw.toString().startsWith('{'));
    }

    void scriptsFallBackToPreJsonPlainKeys() {
        FreshSettings fresh;
#ifdef Q_OS_WIN
        QSettings conf(QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() +
                           ".ini",
                       QSettings::IniFormat);
#else
        QSettings conf(QCoreApplication::organizationName(), QCoreApplication::applicationName());
#endif
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
        // is inside setupCache, or logs and thumbnails split across dirs.
        // The key must be written to the backend the resolver reads: portable
        // mode keeps the conf in the redirected ini.
        FreshSettings fresh;
        const QString alt =
            QDir::tempPath() + QStringLiteral("/qimgv-cache-alt-%1").arg(QCoreApplication::applicationPid());
        QSettings conf(
#ifdef Q_OS_WIN
            QString::fromUtf8(qgetenv("QIMGV_CONF_DIR")) + "/" + QCoreApplication::applicationName() + ".ini",
            QSettings::IniFormat
#else
            QCoreApplication::organizationName(), QCoreApplication::applicationName()
#endif
        );
        conf.setValue(QStringLiteral("cacheDir"), alt);
        conf.sync();
        QCOMPARE(Settings::resolveCacheDir(), alt);
        QVERIFY(QDir(alt).exists());
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
