#ifndef PRINTDIALOG_H
#define PRINTDIALOG_H

#include <QDialog>
#include <QtPrintSupport/QPrinter>
#include <QtPrintSupport/QPrinterInfo>
#include <QFileDialog>
#include <QPainter>
#include <QGraphicsColorizeEffect>
#include <QDebug>
#include "components/thumbnailer/thumbnailer.h"
#include "settings.h"

namespace Ui {
class PrintDialog;
}

class PrintDialog : public QDialog {
    Q_OBJECT

  public:
    explicit PrintDialog(QWidget *parent = nullptr);
    ~PrintDialog() override;
    void setImage(std::shared_ptr<const QImage> _img);
    void setOutputPath(QString path);

  private slots:
    void print();
    void exportPdf();
    QRectF getImagePrintRect(QPrinter *printer);
    void updatePreview();
    void setLandscape(bool mode);
    void onPrinterSelected(const QString &name);
    QString pdfPathDialog();

  private:
    void saveSettings();
    Ui::PrintDialog *m_ui;
    std::shared_ptr<const QImage> m_img = nullptr;
    QPrinter m_pdfPrinter, *m_printer = nullptr;
    bool m_printPdfDefault = false;
};

#endif // PRINTDIALOG_H
