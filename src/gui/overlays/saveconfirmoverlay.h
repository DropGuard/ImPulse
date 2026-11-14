#pragma once

#include "gui/customwidgets/overlaywidget.h"
#include "gui/ipendingsurface.h"
#include "settings.h"
#include <QPushButton>
#include <QKeyEvent>

namespace Ui {
class SaveConfirmOverlay;
}

class SaveConfirmOverlay : public OverlayWidget, public IPendingSurface {
    Q_OBJECT
  public:
    // IPendingSurface: this surface holds unfinished work while visible
    bool isPending() const override {
        return isVisible();
    }
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
