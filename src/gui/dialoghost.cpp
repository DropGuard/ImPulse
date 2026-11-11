#include "gui/dialoghost.h"

#include "gui/mainwindow.h"
#include "gui/viewers/documentwidget.h"
#include "gui/dialogs/resizedialog.h"
#include "gui/dialogs/filereplacedialog.h"
#include "gui/dialogs/settingsdialog.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QImageWriter>

DialogHost::DialogHost(MW *windowArg, DocumentWidget *docWidgetArg)
    : QObject(windowArg), window(windowArg), docWidget(docWidgetArg) {
}

bool DialogHost::showConfirmation(const QString &title, const QString &msg) {
    QMessageBox msgBox(window);
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
    docWidget->hideFloatingPanel();

    QFileDialog dialog(window);
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

void DialogHost::showSaveDialog(const QString &filePath) {
    QString newFilePath = getSaveFileName(filePath);
    if(!newFilePath.isEmpty())
        emit saveAsRequested(newFilePath);
}

QString DialogHost::getSaveFileName(const QString &filePath) {
    docWidget->hideFloatingPanel();
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
    QFileDialog dialog(window);
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
    ResizeDialog dialog(initialSize, window);
    connect(&dialog, &ResizeDialog::sizeSelected, this, &DialogHost::resizeRequested);
    dialog.exec();
}

DialogResult DialogHost::fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode mode,
                                           bool multiple) {
    FileReplaceDialog dialog(window);
    dialog.setModal(true);
    dialog.setSource(src);
    dialog.setDestination(dst);
    dialog.setMode(mode);
    dialog.setMulti(multiple);

    dialog.exec();

    return dialog.getResult();
}

void DialogHost::showSettings() {
    docWidget->hideFloatingPanel();
    SettingsDialog settingsDialog(window);
    settingsDialog.exec();
}

void DialogHost::showScriptSettings() {
    docWidget->hideFloatingPanel();
    SettingsDialog settingsDialog(window);
    settingsDialog.switchToPage(4);
    settingsDialog.exec();
}
