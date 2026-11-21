#pragma once

#include <QVBoxLayout>
#include "gui/customwidgets/slidepanel.h"
#include "gui/customwidgets/actionbutton.h"
#include "gui/panels/mainpanel/thumbnailstrip.h"

class MainPanel : public SlidePanel {
    Q_OBJECT
  public:
    MainPanel(FloatingWidgetContainer *parent);
    void setPosition(PanelPosition);
    void setExitButtonEnabled(bool mode);
    ThumbnailStrip *getThumbnailStrip();
    void setupThumbnailStrip();
    QSize sizeHint() const override;

  public slots:
    void readSettings();

  signals:
    void pinned(bool mode);

  private slots:
    void onPinClicked();

  private:
    QVBoxLayout *m_buttonsLayout = nullptr;
    QWidget m_buttonsWidget;
    ThumbnailStrip *m_thumbnailStrip = nullptr;
    ActionButton *m_openButton, *m_settingsButton, *m_exitButton, *m_folderViewButton, *m_pinButton;

  protected:
    void paintEvent(QPaintEvent *event) override;
};
