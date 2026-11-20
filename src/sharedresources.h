#pragma once

#include <QPixmap>
#include <QDebug>

enum ShrIcon {
    SHR_ICON_ERROR,
    SHR_ICON_LOADING
};

class SharedResources {
  public:
    SharedResources();
    static SharedResources *getInstance();
    ~SharedResources();

    QPixmap *getPixmap(ShrIcon icon, qreal dpr);

  private:
    QPixmap *m_loadingIcon72 = nullptr;
    QPixmap *m_loadingErrorIcon72 = nullptr;
};

extern SharedResources *shrRes;
