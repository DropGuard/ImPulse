#pragma once

#include "gui/customwidgets/overlaywidget.h"
#include "settings.h"
#include <QPushButton>
#include <QKeyEvent>

namespace Ui {
class SaveConfirmOverlay;
}

class SaveConfirmOverlay : public OverlayWidget {
    Q_OBJECT
  public:
    explicit SaveConfirmOverlay(FloatingWidgetContainer *parent = nullptr);
    ~SaveConfirmOverlay() override;

  protected:
    void keyPressEvent(QKeyEvent *event) override;

  signals:
    void saveClicked();
    void saveAsClicked();
    void discardClicked();

  private slots:
    void readSettings();

  private:
    Ui::SaveConfirmOverlay *ui;
};
