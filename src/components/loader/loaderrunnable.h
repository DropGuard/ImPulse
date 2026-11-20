#pragma once

#include <QObject>
#include <optional>
#include "components/workerpool/workerpool.h"
#include "utils/imagefactory.h"

class LoaderRunnable : public QObject, public PoolJob {
    Q_OBJECT
  public:
    LoaderRunnable(QString _path, bool _scaled, const LoadPolicy &policy);
    // the key the owner tracks this job by in its task table
    const QString &loadPath() const {
        return m_path;
    }
    void run() override;

  private:
    QString m_path;
    bool m_scaled;
    LoadPolicy m_policy;
  signals:
    void finished(std::optional<std::shared_ptr<Image>>, QString, bool scaled);
};
