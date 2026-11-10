#pragma once

// Modal dialogs, extracted from MW (the strangler rewrite of mainwindow.cpp):
// open, save-as, resize, settings, script settings, the file-replace dialog
// and the confirmation box. The host owns no state — dialogs are
// stack-allocated per invocation; it only parents them to the window and
// re-emits the user's intent (opened / saveAsRequested / resizeRequested)
// for Core to connect to.

#include <QObject>

#include "gui/dialogs/dialogresult.h"

class MW;
class DocumentWidget;

class DialogHost : public QObject {
    Q_OBJECT
  public:
    explicit DialogHost(MW *window, DocumentWidget *docWidget);

    void showOpenDialog(const QString &path);
    void showSaveDialog(const QString &filePath);
    QString getSaveFileName(const QString &filePath);
    void showResizeDialog(QSize initialSize);
    void showSettings();
    void showScriptSettings();
    bool showConfirmation(const QString &title, const QString &msg);
    DialogResult fileReplaceDialog(const QString &source, const QString &target, FileReplaceMode mode, bool multiple);

  signals:
    void opened(QString);
    void saveAsRequested(QString);
    void resizeRequested(QSize);

  private:
    MW *window;
    DocumentWidget *docWidget;
};
