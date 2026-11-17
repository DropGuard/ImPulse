#pragma once

#include <QDialog>
#include <QFileDialog>
#include <QColorDialog>
#include <QTableWidget>
#include <QTextBrowser>
#include <QListWidget>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QApplication>
#include <QDebug>
#include <QMenu>
#include "gui/customwidgets/colorselectorbutton.h"
#include "gui/dialogs/shortcutcreatordialog.h"
#include "gui/dialogs/scripteditordialog.h"
#include "settings.h"
#include "components/actionmanager/actionmanager.h"

namespace Ui {
class QCheckBox;
class SettingsDialog;
}

class SettingsDialog : public QDialog {
    Q_OBJECT

  public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;
    void switchToPage(int number);

  public slots:
    int exec() override;
    // both directions of the binding table; public so tests can drive them
    void readSettings();
    void saveSettings();
    // rebuilds every scope's table from the rendered rows; public on the
    // same seam as readSettings — the save-split (which row lands in which
    // scope's table) is exactly what the test has to see
    void saveShortcuts();
    // The message the import would show, built without showing it. The test
    // needs the text; QMessageBox::information blocks on a modal dialog that
    // no headless run can dismiss. Public for that seam, not for callers.

  private:
    void readColorScheme();
    void setColorScheme(ColorScheme colors);
    void saveColorScheme();
    void readShortcuts();
    void readScripts();
    Ui::SettingsDialog *ui;

    void addShortcutToTable(const QString &action, const QString &shortcut, Scope scope);
    // the stable registry name a row stands for (the cell shows a display name)
    QString rowAction(int row) const;
    // display name for an action, translated; the raw name for scripts/unknowns
    QString actionDisplayName(const QString &action) const;
    void addSectionHeader(const QString &title);
    // the section title for a scope (the editor's own prose, not the label the
    // conflict warning uses)
    QString sectionTitle(Scope scope) const;
    void addUnboundActionRow(const QString &action, Scope scope);
    void addUnboundRows(Scope scope);
    // false for a section header (no tag) and for a tag this build does not
    // name — both are "not a binding", never a silent drop
    bool rowScope(int row, Scope &out) const;
    Scope selectedScope() const;
    void addScriptToList(const QString &name);

    void setupSidebar();
    void removeShortcutAt(int row);
    void adjustSizeToContents();
    QButtonGroup fitModeGrp, folderEndGrp, zoomIndGrp;

  private:
    struct CheckBinding {
        QCheckBox *box;
        bool (Settings::*get)();
        void (Settings::*set)(bool);
    };
    QVector<CheckBinding> mChecks;

    struct ComboBinding {
        QComboBox *box;
        const SettingDesc *desc;
        QStringList values; // combo index -> stored value
        QString fallback;   // used when the stored value is not in the list
    };
    QVector<ComboBinding> mCombos;

  private slots:
    void saveSettingsAndClose();

    void addScript();
    void editScript();
    void editScript(QListWidgetItem *item);
    void editScript(const QString &name);
    void removeScript();

    void addShortcut();
    void editShortcut();
    void editShortcut(int row);
    void removeShortcut();
    void resetShortcuts();
    void selectMpvPath();
    // Explicit, never automatic: until 2026.11 this fork and upstream qimgv
    // shared the "qimgv/qimgv" identity, so the directory may belong to an
    // upstream installation. The user asks, the dialog reports what happened.
    void onBgOpacitySliderChanged(int value);
    void onThumbnailerThreadsSliderChanged(int value);
    void onExpandLimitSliderChanged(int value);
    void onZoomStepSliderChanged(int value);
    void onJPEGQualitySliderChanged(int value);
    void resetToDesktopTheme();
    void onAutoResizeLimitSliderChanged(int value);
    void onMouseScrollingSpeedSliderChanged(int value);

    void resetZoomLevels();
  signals:
    void settingsChanged();
};
