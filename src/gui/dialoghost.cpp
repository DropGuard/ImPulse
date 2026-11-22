#include "gui/dialoghost.h"

#include "gui/mainwindow.h"
#include "gui/viewers/documentwidget.h"
#include "gui/dialogs/resizedialog.h"
#include "gui/dialogs/filereplacedialog.h"
#include "gui/dialogs/settingsdialog.h"
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QImageWriter>

DialogHost::DialogHost(MW *windowArg, DocumentWidget *docWidgetArg)
    : QObject(windowArg), m_window(windowArg), m_docWidget(docWidgetArg) {
}

bool DialogHost::showConfirmation(const QString &title, const QString &msg) {
    QMessageBox msgBox(m_window);
    msgBox.setWindowTitle(title);
    msgBox.setText(msg);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Yes);
    msgBox.addButton(QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::Yes);
    msgBox.setModal(true);
    if(msgBox.exec() == QMessageBox::Yes)
        return true;
    else
        return false;
}

void DialogHost::showOpenDialog(const QString &path) {
    m_docWidget->hideFloatingPanel();

    QFileDialog dialog(m_window);
    QStringList imageFilter;
    imageFilter.append(settings->supportedFormatsFilter());
    imageFilter.append("All Files (*)");
    dialog.setDirectory(path);
    dialog.setNameFilters(imageFilter);
    dialog.setWindowTitle("Open image");
    dialog.setWindowModality(Qt::ApplicationModal);
    connect(&dialog, &QFileDialog::fileSelected, this, &DialogHost::opened);
    dialog.exec();
}

int DialogHost::chooseEditDraft(const QStringList &labels) {
    bool accepted = false;
    const auto item = QInputDialog::getItem(m_window, tr("Recover edits"), tr("Choose a draft to export:"), labels, 0,
                                            false, &accepted);
    return accepted ? labels.indexOf(item) : -1;
}

QString DialogHost::getRecoveryFileName(const QString &suggested) {
    QFileDialog dialog(m_window, tr("Export recovered edit"), suggested, tr("PNG (*.png)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("png"));
    return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0) : QString();
}

QString DialogHost::getSaveFileName(const QString &filePath) {
    m_docWidget->hideFloatingPanel();
    QStringList filters;
    // generate filter for writable images
    // todo: some may need to be blacklisted
    auto writerFormats = QImageWriter::supportedImageFormats();
    if(writerFormats.contains("jpg"))
        filters.append("JPEG (*.jpg *.jpeg *jpe *jfif)");
    if(writerFormats.contains("png"))
        filters.append("PNG (*.png)");
    if(writerFormats.contains("webp"))
        filters.append("WebP (*.webp)");
    // may not work..
    if(writerFormats.contains("jp2"))
        filters.append("JPEG 2000 (*.jp2 *.j2k *.jpf *.jpx *.jpm *.jpgx)");
    if(writerFormats.contains("jxl"))
        filters.append("JPEG-XL (*.jxl)");
    if(writerFormats.contains("avif"))
        filters.append("AVIF (*.avif *.avifs)");
    if(writerFormats.contains("tif"))
        filters.append("TIFF (*.tif *.tiff)");
    if(writerFormats.contains("bmp"))
        filters.append("BMP (*.bmp)");
#ifdef _WIN32
    if(writerFormats.contains("ico"))
        filters.append("Icon Files (*.ico)");
#endif
    if(writerFormats.contains("ppm"))
        filters.append("PPM (*.ppm)");
    if(writerFormats.contains("xbm"))
        filters.append("XBM (*.xbm)");
    if(writerFormats.contains("xpm"))
        filters.append("XPM (*.xpm)");
    if(writerFormats.contains("dds"))
        filters.append("DDS (*.dds)");
    if(writerFormats.contains("wbmp"))
        filters.append("WBMP (*.wbmp)");
    // add everything else from imagewriter
    for(const auto &fmt : std::as_const(writerFormats)) {
        if(filters.filter(fmt).isEmpty())
            filters.append(fmt.toUpper() + " (*." + fmt + ")");
    }

    // find matching filter for the current image
    QString selectedFilter = "JPEG (*.jpg *.jpeg *jpe *jfif)";
    QFileInfo fi(filePath);
    for(const QString &filter : std::as_const(filters)) {
        if(filter.contains(fi.suffix().toLower())) {
            selectedFilter = filter;
            break;
        }
    }
    // the prefill is explicit, not an accident of backend: a full file path
    // in the `dir` slot of the static getSaveFileName happens to prefill on
    // some platform dialogs and is dropped by others (the portal backends
    // have a history of it). setDirectory + selectFile is the documented
    // form every backend forwards.
    QFileDialog dialog(m_window);
    dialog.setWindowTitle(tr("Save File as..."));
    dialog.setDirectory(fi.absolutePath());
    dialog.selectFile(fi.fileName());
    dialog.setNameFilters(filters);
    dialog.selectNameFilter(selectedFilter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setWindowModality(Qt::ApplicationModal);
    QString newFilePath;
    if(dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty())
        newFilePath = dialog.selectedFiles().constFirst();
    return newFilePath;
}

void DialogHost::showResizeDialog(QSize initialSize) {
    ResizeDialog dialog(initialSize, m_window);
    connect(&dialog, &ResizeDialog::sizeSelected, this, &DialogHost::resizeRequested);
    dialog.exec();
}

DialogResult DialogHost::fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode mode,
                                           bool multiple) {
    FileReplaceDialog dialog(m_window);
    dialog.setModal(true);
    dialog.setSource(src);
    dialog.setDestination(dst);
    dialog.setMode(mode);
    dialog.setMulti(multiple);

    dialog.exec();

    return dialog.getResult();
}

void DialogHost::showSettings() {
    m_docWidget->hideFloatingPanel();
    SettingsDialog settingsDialog(m_window);
    settingsDialog.exec();
}

void DialogHost::showScriptSettings() {
    m_docWidget->hideFloatingPanel();
    SettingsDialog settingsDialog(m_window);
    settingsDialog.switchToPage(4);
    settingsDialog.exec();
}
