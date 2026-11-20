#pragma once
#include <QDialog>
#include "gui/dialogs/dialogresult.h"

namespace Ui {
class FileReplaceDialog;
}

class FileReplaceDialog : public QDialog {
    Q_OBJECT

  public:
    explicit FileReplaceDialog(QWidget *parent = nullptr);
    ~FileReplaceDialog() override;

    void setMode(FileReplaceMode mode);
    void setMulti(bool);
    DialogResult getResult();

    void setSource(const QString &src);
    void setDestination(const QString &dst);
  private slots:
    void onYesClicked();
    void onNoClicked();
    void onCancelClicked();

  private:
    Ui::FileReplaceDialog *m_ui;
    bool m_multi;
    DialogResult m_result;
};
