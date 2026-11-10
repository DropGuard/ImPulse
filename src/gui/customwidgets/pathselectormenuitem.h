#pragma once

#include <QFileDialog>
#include "gui/customwidgets/menuitem.h"
#include "settings.h"

class PathSelectorMenuItem : public MenuItem {
    Q_OBJECT
  public:
    PathSelectorMenuItem(QWidget *parent);
    const QString &directory();
    const QString &path();

  public slots:
    void setDirectory(const QString &path);

  protected:
    void onPress() override;

  signals:
    void directorySelected(QString);

  private:
    QString mDirectory;
    QString mPath;
};
