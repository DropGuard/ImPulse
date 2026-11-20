#include "printdialog.h"

#include <utility>
#include "ui_printdialog.h"

PrintDialog::PrintDialog(QWidget *parent) : QDialog(parent), m_ui(new Ui::PrintDialog) {
    m_ui->setupUi(this);
    m_ui->previewLabel->setContentsMargins(0, 0, 0, 0);
    m_pdfPrinter.setOutputFormat(QPrinter::PdfFormat);
    m_pdfPrinter.setPageSize(QPageSize(QPageSize::A4));
    m_pdfPrinter.setOutputFileName(" ");
    QStringList printerList = QPrinterInfo::availablePrinterNames();
    if(printerList.isEmpty()) {
        m_ui->printerListComboBox->hide();
        m_ui->printButton->setEnabled(false);
        m_ui->exportPdfButton->setFocus();
    } else {
        m_ui->printerListPlaceholder->hide();
        m_ui->printerListComboBox->addItems(printerList);
        m_ui->printerListComboBox->setCurrentText(QPrinterInfo::defaultPrinterName());
        if(printerList.contains(settings->lastPrinter()))
            onPrinterSelected(settings->lastPrinter());
        else
            onPrinterSelected(QPrinterInfo::defaultPrinterName());
        m_printPdfDefault = settings->printPdfDefault();
    }
    m_ui->color->setChecked(settings->printColor());
    setLandscape(settings->printLandscape());
    m_ui->fitToPageCheckBox->setChecked(settings->printFitToPage());
    if(m_printPdfDefault)
        m_ui->exportPdfButton->setFocus();
    // ui signals
    connect(m_ui->cancelButton, &QPushButton::clicked, this, &QWidget::close);
    connect(m_ui->printButton, &QPushButton::clicked, this, &PrintDialog::print);
    connect(m_ui->exportPdfButton, &QPushButton::clicked, this, &PrintDialog::exportPdf);
    connect(m_ui->printerListComboBox, &QComboBox::currentTextChanged, this, &PrintDialog::onPrinterSelected);
    connect(m_ui->landscape, &QRadioButton::toggled, this, &PrintDialog::setLandscape);
    connect(m_ui->fitToPageCheckBox, &QCheckBox::toggled, this, &PrintDialog::updatePreview);
    connect(m_ui->color, &QRadioButton::toggled, this, &PrintDialog::updatePreview);
}

void PrintDialog::saveSettings() {
    settings->setPrintLandscape(m_ui->landscape->isChecked());
    settings->setPrintColor(m_ui->color->isChecked());
    settings->setPrintFitToPage(m_ui->fitToPageCheckBox->isChecked());
    settings->setPrintPdfDefault(m_printPdfDefault);
    if(!m_ui->printerListComboBox->currentText().isEmpty())
        settings->setLastPrinter(m_ui->printerListComboBox->currentText());
}

PrintDialog::~PrintDialog() {
    saveSettings();
    if(m_printer)
        delete m_printer;
    delete m_ui;
}

void PrintDialog::setImage(std::shared_ptr<const QImage> _img) {
    m_img = std::move(_img);
    updatePreview();
}

void PrintDialog::setOutputPath(QString path) {
    if(path.isEmpty())
        path = " ";
    m_pdfPrinter.setOutputFileName(path);
}

QString PrintDialog::pdfPathDialog() {
    return QFileDialog::getSaveFileName(this, tr("Choose pdf location"), m_pdfPrinter.outputFileName(), "*.pdf");
}

void PrintDialog::updatePreview() {
    if(!m_img)
        return;
    QPrinter *targetPrinter = m_printer;
    if(!targetPrinter)
        targetPrinter = &m_pdfPrinter;
    auto imgRect = getImagePrintRect(targetPrinter);
    QRectF fullRect = targetPrinter->pageLayout().fullRectPixels(targetPrinter->resolution());
    // margins
    QMarginsF margins(targetPrinter->pageLayout().marginsPixels(targetPrinter->resolution()));
    // scaled page with margins
    QRect fullRectScaled(
        QRectF(QPointF(0, 0), fullRect.size().scaled(m_ui->previewLabel->size(), Qt::KeepAspectRatio)).toRect());
    qreal scale = fullRectScaled.width() / fullRect.width();
    // scaled image rect with margins (not accurate, but good enough for a preview)
    QRect imgRectScaled(QRectF((imgRect.left() + margins.left()) * scale, (imgRect.top() + margins.top()) * scale,
                               imgRect.width() * scale, imgRect.height() * scale)
                            .toRect());
    QPixmap pagePixmap(fullRectScaled.size() * qApp->devicePixelRatio());
    pagePixmap.setDevicePixelRatio(qApp->devicePixelRatio());
    auto scaledImg =
        m_img->scaled(imgRectScaled.size() * qApp->devicePixelRatio(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if(m_ui->grayscale->isChecked())
        scaledImg = scaledImg.convertToFormat(QImage::Format_Grayscale8);
    scaledImg.setDevicePixelRatio(qApp->devicePixelRatio());
    QPainter p(&pagePixmap);
    p.fillRect(pagePixmap.rect(), QColor(255, 255, 255));
    p.drawImage(imgRectScaled.left(), imgRectScaled.top(), scaledImg);
    // page border for white window bg
    QPalette palette;
    QColor sys_window = palette.window().color();
    if(sys_window.valueF() > 0.45f) {
        p.setOpacity(0.25f);
        p.setPen(Qt::black);
        p.drawRect(
            QRectF(QPointF(0.5f, 0.5f), QSizeF(pagePixmap.size() / qApp->devicePixelRatio() - QSizeF(1.0f, 1.0f))));
    }
    m_ui->previewLabel->setPixmap(pagePixmap);
}

QRectF PrintDialog::getImagePrintRect(QPrinter *pr) {
    QRectF imgRect;
    if(!pr || !m_img)
        return QRect();
    QRectF pageRect = QRectF(QPoint(0, 0), pr->pageRect(QPrinter::DevicePixel).size());
    imgRect = m_img->rect();
    // downscale / upscale
    if(m_ui->fitToPageCheckBox->isChecked() || imgRect.width() > pageRect.width() ||
       imgRect.height() > pageRect.height())
        imgRect.setSize(imgRect.size().scaled(pageRect.size(), Qt::KeepAspectRatio));
    // align top center
    imgRect.moveCenter(pageRect.center());
    imgRect.moveTop(pageRect.top());
    return imgRect;
}

void PrintDialog::setLandscape(bool mode) {
    m_ui->landscape->blockSignals(true);
    m_ui->landscape->setChecked(mode);
    m_ui->landscape->blockSignals(false);
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    if(mode)
        orientation = QPageLayout::Landscape;
    if(m_printer)
        m_printer->setPageOrientation(orientation);
    m_pdfPrinter.setPageOrientation(orientation);
    updatePreview();
}

void PrintDialog::onPrinterSelected(const QString &name) {
    if(m_printer)
        delete m_printer;
    m_printer = new QPrinter(QPrinterInfo::printerInfo(name));
    updatePreview();
}

void PrintDialog::print() {
    if(!m_img || !m_printer) {
        close();
        return;
    }
    if(m_ui->color->isChecked())
        m_printer->setColorMode(QPrinter::Color);
    else
        m_printer->setColorMode(QPrinter::GrayScale);
    QPainter p(m_printer);
    p.drawImage(getImagePrintRect(m_printer), *m_img);
    m_printPdfDefault = false;
    close();
}

void PrintDialog::exportPdf() {
    if(!m_img || m_pdfPrinter.outputFileName().isEmpty()) {
        close();
        return;
    }
    auto path = pdfPathDialog();
    if(path.isEmpty())
        return;
    m_pdfPrinter.setOutputFileName(path);
    if(m_ui->color->isChecked())
        m_pdfPrinter.setColorMode(QPrinter::Color);
    else
        m_pdfPrinter.setColorMode(QPrinter::GrayScale);
    QPainter p(&m_pdfPrinter);
    p.drawImage(getImagePrintRect(&m_pdfPrinter), *m_img);
    m_printPdfDefault = true;
    close();
}
