#pragma once

#include <QPushButton>
#include <QLineEdit>
#include <QKeyEvent>
#include <QTimer>

#include "gui/customwidgets/overlaywidget.h"
#include "gui/ipendingsurface.h"
#include "components/actionmanager/actionmanager.h"
#include "settings.h"

namespace Ui {
class RenameOverlay;
}

class RenameOverlay : public OverlayWidget, public IPendingSurface {
    Q_OBJECT

  public:
    // IPendingSurface: this surface holds unfinished work while visible
    bool isPending() const override {
        return isVisible();
    }
    explicit RenameOverlay(FloatingWidgetContainer *parent);
    ~RenameOverlay() override;

  public slots:
    void setName(const QString &name);
    void setBackdropEnabled(bool mode);
    void show();
    void hide();
  signals:
    void renameRequested(QString name);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void recalculateGeometry() override;

  private slots:
    void rename();
    void onCancel();

  private:
    Ui::RenameOverlay *m_ui;
    bool m_backdrop = false;
    QString m_origName;
    QList<QString> m_keyFilter;
    QList<QKeySequence> m_keyFilterSequences;
    void selectName();
};
