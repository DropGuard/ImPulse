#pragma once

#include <QGridLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QKeyEvent>
#include "gui/customwidgets/overlaywidget.h"
#include "gui/ipendingsurface.h"
#include "gui/customwidgets/pathselectormenuitem.h"
#include "settings.h"
#include "components/actionmanager/actionmanager.h"

enum CopyOverlayMode {
    OVERLAY_COPY,
    OVERLAY_MOVE
};

namespace Ui {
class CopyOverlay;
}

class CopyOverlay : public OverlayWidget, public IPendingSurface {
    Q_OBJECT
  public:
    // IPendingSurface: this surface holds unfinished work while visible
    bool isPending() const override {
        return isVisible();
    }
    CopyOverlay(FloatingWidgetContainer *parent);
    ~CopyOverlay() override;
    void saveSettings();
    void setDialogMode(CopyOverlayMode _mode);
    CopyOverlayMode operationMode();

  public slots:
    void show();
    void hide();

  signals:
    void copyRequested(QString);
    void moveRequested(QString);

  protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    // clicking anywhere but the menu dismisses it — the same answer Esc gives,
    // and the same click-outside rule RenameOverlay already follows. Without
    // it the only way out was the close button or a key, and a click on the
    // image behind looked like it was being ignored.
    void mousePressEvent(QMouseEvent *event) override;

    bool focusNextPrevChild(bool) override;
  private slots:
    void requestFileOperation(const QString &path);
    void readSettings();

  private:
    void createDefaultPaths();
    void createPathWidgets();
    void createShortcuts();
    Ui::CopyOverlay *m_ui;
    QList<PathSelectorMenuItem *> m_pathWidgets;
    const int m_maxPathCount = 9;
    QStringList m_paths;
    QMap<QString, int> m_shortcuts;
    CopyOverlayMode m_mode;
    void removePathWidgets();
};
