/*
 * Core's clipboard and drag-and-drop half: the app as a mime source and
 * sink — clipboard writes, drop-ins, drag-outs and the mime payload
 * builders. Same class, partial implementation — core.cpp keeps
 * composition, wiring and the display sink.
 */

#include "core.h"
#include <utility>

#include "utils/folderend.h"

#include "gui/windowshell.h"
#include "gui/overlayhost.h"
#include "gui/dialoghost.h"
#include "gui/crop/cropcontroller.h"

#include <QThread>
#include <QDrag>
#include <QTemporaryFile>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#ifdef __WIN32
#include <tchar.h>
#endif

// TODO: also copy selection from folder view?
void Core::copyFileClipboard() {
    if(model->isEmpty())
        return;

    QMimeData *mimeData = getMimeDataForImage(model->getImage(selectedPath()), TARGET_CLIPBOARD);

    // mimeData->text() should already contain an url
    QByteArray gnomeFormat = QByteArray("copy\n").append(QUrl(mimeData->text()).toEncoded());
    mimeData->setData("x-special/gnome-copied-files", gnomeFormat);
    mimeData->setData("application/x-kde-cutselection", "0");

    QApplication::clipboard()->setMimeData(mimeData);
    overlays_->showMessage(tr("File copied"));
}

void Core::copyPathClipboard() {
    if(model->isEmpty())
        return;
    QApplication::clipboard()->setText(selectedPath());
    overlays_->showMessage(tr("Path copied"));
}

// open from clipboard
// todo: actual file paste into folderview (like filemanager)
// The folder view's Ctrl+V. The clipboard's urls are the files to place; the
// model's directory is where. interactiveCopy is the same path the copy
// overlay uses, so overwrite prompts, error decoding and the watcher's
// refresh are all the ones already exercised — this command adds no file
// machinery of its own, it only answers "which paths, which directory".
//
// Deliberately skipped rather than reported:
//   - a remote url (toLocalFile() is empty) — the same refusal
//     openFromClipboard makes, for the same reason: only local files exist
//     for this application;
//   - a file already in the destination directory — copying a file onto
//     itself is not a user intent, and interactiveCopy would raise an
//     overwrite prompt for an operation that can only produce an identical
//     file.
void Core::pasteFilesToCurrentDirectory() {
    const QString destDir = model->directoryPath();
    if(destDir.isEmpty())
        return; // no directory to paste into
    const QMimeData *mimeData = QApplication::clipboard()->mimeData();
    if(!mimeData || !mimeData->hasUrls())
        return;
    QList<QString> paths;
    const QDir dest(destDir);
    const QList<QUrl> urls = mimeData->urls(); // a temp QList in the range-for detaches per iteration
    for(const QUrl &url : urls) {
        const QString path = url.toLocalFile();
        if(path.isEmpty())
            continue; // remote: not this application's kind of file
        if(QFileInfo(path).absolutePath() == dest.absolutePath())
            continue; // already here
        paths << path;
    }
    if(paths.isEmpty())
        return;
    interactiveCopy(paths, destDir);
}

void Core::openFromClipboard() {
    auto cb = QApplication::clipboard();
    auto mimeData = cb->mimeData();
    if(!mimeData)
        return;

    // try opening url
    if(mimeData->hasUrls()) {
        const QUrl url = mimeData->urls().value(0);
        QString path = url.toLocalFile();
        if(path.isEmpty()) {
            qDebug() << "Could not load url:" << url;
            qDebug() << "Currently only local files are supported.";
        } else if(loadPath(path)) {
            return;
        }
    }

    // try to save the image buffer, then open what was written
    if(mimeData->hasImage()) {
        const QImage image = cb->image();
        if(image.isNull())
            return;
        QString destPath = (!model->isEmpty() ? model->directoryPath() : QDir::homePath());
        destPath.append("/clipboard.png");
        destPath = dialogs_->getSaveFileName(destPath);
        if(destPath.isEmpty())
            return;
        // the same save-with-rollback the on-disk editors use; this used to be
        // a copy of it, and the copy kept the rollback path that discarded the
        // backup even when restoring it had failed
        if(FileOperations::saveWithBackup(image, destPath, settings->JPEGSaveQuality()))
            loadPath(destPath);
    }
}

void Core::onDropIn(const QMimeData *mimeData, QObject *source) {
    // ignore self
    if(source == this)
        return;
    // check for our needed mime type, here a file or a list of files
    if(mimeData->hasUrls()) {
        QStringList pathList;
        QList<QUrl> urlList = mimeData->urls();
        // extract the local paths of the files
        for(int i = 0; i < urlList.size(); ++i)
            pathList.append(urlList.at(i).toLocalFile());
        // try to open first file in the list
        loadPath(pathList.first());
    }
}

// drag'n'drop
// drag image out of the program
void Core::onDraggedOut() {
    onDraggedOut(currentSelection());
}

QMimeData *Core::mimeDataForPaths(const QList<QString> &paths) {
    QMimeData *mimeData;
    // single selection, image
    if(paths.count() == 1 && model->containsFile(paths.first())) {
        mimeData = getMimeDataForImage(model->getImage(paths.last()), TARGET_DROP);
        // the Image object may simply not be in the cache — an entry in the
        // listing is a file whether or not its full-size image was loaded.
        // An empty QMimeData makes every drop target refuse the drag with a
        // forbidden cursor for its entire lifetime.
        if(!mimeData->hasUrls())
            mimeData->setUrls({QUrl::fromLocalFile(paths.first())});
    } else { // multi-selection, or single directory. drag urls
        mimeData = new QMimeData();
        QList<QUrl> urlList;
        for(const auto &path : paths)
            urlList << QUrl::fromLocalFile(path);
        mimeData->setUrls(urlList);
    }
    return mimeData;
}

void Core::onDraggedOut(const QList<QString> &paths) {
    if(paths.isEmpty())
        return;
    //auto thumb = Thumbnailer::getThumbnail(paths.last(), 100);
    // exec() blocks for the whole gesture, so the drag handle is scoped to this
    // call: the QMimeData it owns goes with it. A member here would leave every
    // past drag's payload parented to Core for the life of the session.
    QDrag drag(this);
    drag.setMimeData(mimeDataForPaths(paths));
    //drag.setPixmap(*thumb->pixmap().get());
    drag.exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
}

QMimeData *Core::getMimeDataForImage(std::shared_ptr<Image> img, MimeDataTarget target) {
    QMimeData *mimeData = new QMimeData();
    if(!img)
        return mimeData;
    QString path = img->filePath();
    if(img->type() == STATIC) {
        if(img->isEdited()) {
            // An edited image has no file on disk: the export hands the
            // consumer a rendering of the edits. That file used to be one
            // fixed name, tmpDir/image.png — a drag in flight and a clipboard
            // entry from two exports shared it, and one consumer read the
            // other's pixels. The file is now a uniquely-named QTemporaryFile
            // parented to the QMimeData it serves: it lives exactly as long
            // as the drag gesture or the clipboard entry does, and the
            // autoremove takes it away with that — the "cleanup temp files"
            // TODO was about this path, and it is now true by construction.
            auto *tmp = new QTemporaryFile(QDir(settings->tmpDir()).filePath("image-XXXXXX.png"), mimeData);
            tmp->setAutoRemove(true);
            // use faster compression for drag'n'drop
            int pngQuality = (target == TARGET_DROP) ? 80 : 30;
            if(tmp->open()) {
                img->getImage()->save(tmp, "png", pngQuality);
                path = tmp->fileName();
            } else {
                // no writable tmp: fall back to the old fixed name rather
                // than turning an export into a failure
                qWarning() << "[mime] edited-image temp file did not open:" << tmp->errorString();
                path = settings->tmpDir() + "image.png";
                img->getImage()->save(path, nullptr, pngQuality);
            }
        }
    }
    // !!! using setImageData() while doing drag'n'drop hangs Xorg !!!
    // clipboard only!
    if(img->type() != VIDEO && target == TARGET_CLIPBOARD)
        mimeData->setImageData(*img->getImage().get());
    mimeData->setUrls({QUrl::fromLocalFile(path)});
    return mimeData;
}
