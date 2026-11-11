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
    // The message the import would show, built without showing it. The test
    // needs the text; QMessageBox::information blocks on a modal dialog that
    // no headless run can dismiss. Public for that seam, not for callers.
    QString importMessage(const QString &sourceDir, const QString &destinationDir);

  private:
    void readColorScheme();
    void setColorScheme(ColorScheme colors);
    void saveColorScheme();
    void readShortcuts();
    void readScripts();
    Ui::SettingsDialog *ui;

    void saveShortcuts();
    void addShortcutToTable(const QString &action, const QString &shortcut);
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
    void importQimgvConfig();
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
