/*
 * Core's file-operations half: delete/trash, rename, copy and move (quick and
 * interactive), the clipboard legs and drag-and-drop. Same class, partial
 * implementation — core.cpp keeps composition, wiring and the display sink.
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

void Core::removePermanent() {
    const auto paths = currentSelection();
    if(!paths.count())
        return;
    // unconditional: the confirmDelete setting is gone — an unrecoverable
    // action never runs silently, whatever the config carries
    QString msg;
    if(paths.count() > 1)
        msg = tr("Delete ") + QString::number(paths.count()) + tr(" items permanently?");
    else
        msg = tr("Delete item permanently?");
    if(!ui_->showConfirmation(tr("Delete permanently"), msg))
        return;
    FileOpResult result;
    int successCount = 0;
    for(const auto &path : paths) {
        QFileInfo fi(path);
        if(fi.isDir())
            model->removeDir(path, false, true, result);
        else
            result = removeFile(path, false);
        if(result == FileOpResult::SUCCESS)
            successCount++;
        else
            qDebug() << "remove failed:" << path << FileOperations::decodeResult(result);
    }
    if(paths.count() == 1) {
        if(result == FileOpResult::SUCCESS)
            overlays_->showMessageSuccess(tr("File removed"));
        else
            outputError(result);
    } else if(paths.count() > 1) {
        reportBatchRemoval(tr("Removed: "), paths.count(), successCount);
    }
}

void Core::moveToTrash() {
    const auto paths = currentSelection();
    if(!paths.count())
        return;
    if(settings->confirmTrash()) {
        QString msg;
        if(paths.count() > 1)
            msg = tr("Move ") + QString::number(paths.count()) + tr(" items to trash?");
        else
            msg = tr("Move item to trash?");
        if(!ui_->showConfirmation(tr("Move to trash"), msg))
            return;
    }
    FileOpResult result;
    int successCount = 0;
    for(const auto &path : paths) {
        result = removeFile(path, true);
        if(result == FileOpResult::SUCCESS)
            successCount++;
        else
            qDebug() << "trash failed:" << path << FileOperations::decodeResult(result);
    }
    if(paths.count() == 1) {
        if(result == FileOpResult::SUCCESS)
            overlays_->showMessageSuccess(tr("Moved to trash"));
        else
            outputError(result);
    } else if(paths.count() > 1) {
        reportBatchRemoval(tr("Moved to trash: "), paths.count(), successCount);
    }
}

void Core::reloadImage() {
    reloadImage(selectedPath());
}

void Core::reloadImage(const QString &filePath) {
    if(model->isEmpty())
        return;
    model->reload(filePath);
}

void Core::renameCurrentSelection(const QString &newName) {
    // Nothing is armed here: the watcher pairs the move into one Renamed event,
    // and Core::onFileRenamed follows it for the file on screen — an in-app
    // rename and an external one take the same path and get the same result.
    const QString oldPath = selectedPath();
    if(!model->fileCount() || newName.isEmpty() || oldPath.isEmpty())
        return;
    FileOpResult result;
    model->renameEntry(oldPath, newName, false, result);
    if(result == FileOpResult::DESTINATION_DIR_EXISTS) {
        overlays_->toggleRenameOverlay(newName);
    } else if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
        if(ui_->showConfirmation(tr("File exists"), tr("Overwrite file?"))) {
            model->renameEntry(oldPath, newName, true, result);
        } else {
            // show rename dialog again
            overlays_->toggleRenameOverlay(newName);
        }
    }
    outputError(result);
}

FileOpResult Core::removeFile(const QString &filePath, bool trash) {
    if(model->isEmpty())
        return FileOpResult::NOTHING_TO_DO;

    bool reopen = false;
    std::shared_ptr<Image> img;
    if(state.currentFilePath == filePath) {
        img = model->getImage(filePath);
        if(img->type() == ANIMATED || img->type() == VIDEO) {
            view_->closeImage();
            reopen = true;
        }
    }
    FileOpResult result;
    model->removeFile(filePath, trash, result);
    if(result != FileOpResult::SUCCESS && reopen)
        guiSetImage(img);
    return result;
}

void Core::outputError(const FileOpResult &error) const {
    if(error == FileOpResult::SUCCESS || error == FileOpResult::NOTHING_TO_DO)
        return;
    overlays_->showError(FileOperations::decodeResult(error));
    qDebug() << FileOperations::decodeResult(error);
}

// A batch reports its failures, not just its successes (#467): a selection
// where some members could not be removed used to show only the success
// count, and the refusals vanished. Both numbers in one message — a second
// call would replace the first — error-styled whenever anything failed, so
// the success styling keeps meaning "all of it worked".
void Core::reportBatchRemoval(const QString &doneVerb, int total, int successCount) const {
    const int failedCount = total - successCount;
    if(failedCount == 0)
        overlays_->showMessageSuccess(doneVerb + QString::number(successCount) + tr(" files"));
    else
        overlays_->showError(doneVerb + QString::number(successCount) + tr(" files, ") + QString::number(failedCount) +
                             tr(" failed"));
}

void Core::showOpenDialog() {
    dialogs_->showOpenDialog(model->directoryPath());
}

void Core::showInDirectory() {
    if(!model)
        return;
#if defined(__linux__) || defined(__FreeBSD__)
    // org.freedesktop.FileManager1 is the whole mechanism: the session's
    // real file manager shows and selects, whatever it is — on the author's
    // machine the name is owned by a running dolphin while the activation
    // file launches nautilus, so no application-side guess can track it.
    // The mime query cannot be trusted either: when this app is itself the
    // inode/directory default it answers "impulse", and the hardcoded
    // fallback ladder then made folder view recurse into itself — twice.
    // An empty selection reveals the directory itself (selected in its
    // parent); a session without the contract gets a message, not a list
    // of guessed binaries.
    QString reveal = selectedPath().isEmpty() ? model->directoryPath() : selectedPath();
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    // QStringList, not QList<QUrl>: QtDBus has no marshaller for the latter,
    // the call died before reaching the bus
    msg.setArguments({QStringList{QUrl::fromLocalFile(reveal).toString()}, QString()});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<> reply = *w;
        if(reply.isError())
            overlays_->showMessage(tr("No file manager is available to reveal the file"));
    });
#elif __WIN32
    QStringList args;
    args << "/select," << QDir::toNativeSeparators(selectedPath());
    QProcess::startDetached("explorer", args);
#elif __APPLE__
    QStringList args;
    args << "-e";
    args << "tell application \"Finder\"";
    args << "-e";
    args << "activate";
    args << "-e";
    args << "select POSIX file \"" + selectedPath() + "\"";
    args << "-e";
    args << "end tell";
    QProcess::startDetached("osascript", args);
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(model->directoryPath()));
#endif
}

// upstream issue #636, promoted from the author's own external script: one
// keypress copies the current file into the configured folder — no dialog,
// no clobbering. Unconfigured, it says so instead of guessing.
void Core::quickCopyToFixedDirectory() {
    if(!model || model->isEmpty() || selectedPath().isEmpty())
        return;
    QString destDir = settings->quickCopyDirectory();
    if(destDir.isEmpty()) {
        overlays_->showMessage(tr("Set the quick copy folder in settings first"));
        return;
    }
    QFileInfo srcFi(selectedPath());
    if(!srcFi.isFile()) {
        overlays_->showMessage(tr("Quick copy: not a regular file"));
        return;
    }
    QDir(destDir).mkpath(".");
    if(QFileInfo::exists(destDir + "/" + srcFi.fileName())) {
        overlays_->showMessage(tr("Already in the quick copy folder"));
        return;
    }
    FileOpResult result;
    FileOperations::copyFileTo(selectedPath(), destDir, false, result);
    if(result == FileOpResult::SUCCESS)
        overlays_->showMessage(tr("Copied to ") + destDir);
    else
        overlays_->showError(FileOperations::decodeResult(result));
}

void Core::interactiveCopy(const QList<QString> &paths, const QString &destDirectory) {
    DialogResult overwriteFiles;
    for(const auto &path : paths) {
        doInteractiveCopy(path, destDirectory, overwriteFiles);
        if(overwriteFiles.cancel)
            return;
    }
}

// todo: replacing DIR with a FILE?
void Core::doInteractiveCopy(const QString &path, const QString &destDirectory, DialogResult &overwriteFiles) {
    QFileInfo srcFi(path);
    // SINGLE FILE COPY ===========================================================================
    if(!srcFi.isDir()) {
        FileOpResult result;
        FileOperations::copyFileTo(path, destDirectory, overwriteFiles, result);
        if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
            if(overwriteFiles.all) // skipping all
                return;
            overwriteFiles = ui_->fileReplaceDialog(srcFi.absoluteFilePath(), destDirectory + "/" + srcFi.fileName(),
                                                    FILE_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            FileOperations::copyFileTo(path, destDirectory, true, result);
        }
        if(result != FileOpResult::SUCCESS && !(result == FileOpResult::DESTINATION_FILE_EXISTS && !overwriteFiles)) {
            overlays_->showError(FileOperations::decodeResult(result));
            qDebug() << FileOperations::decodeResult(result);
        }
        if(!overwriteFiles.all) // copy attempt done; reset temporary flag
            overwriteFiles.yes = false;
        return;
    }
    // DIR COPY (RECURSIVE) =======================================================================
    QDir srcDir(srcFi.absoluteFilePath());
    // a directory has no extension to re-attach: baseName() here truncated
    // every dotted name at its first dot ("1. What" -> "1") — upstream
    // issue #612's corruption signature. fileName() keeps the whole name.
    QFileInfo dstFi(destDirectory + "/" + srcFi.fileName());
    QDir dstDir(dstFi.absoluteFilePath());
    // moving a directory into itself (or its own subtree) recurses forever,
    // each pass re-entering the tree it is copying — issue #612's second
    // bug, the one that shredded 5000 directories. Canonical paths, so
    // symlinks and ../ cannot sneak a self-move past the check.
    QString srcCanon = srcDir.canonicalPath();
    QString dstCanon = dstDir.canonicalPath();
    if(dstCanon.isEmpty())
        dstCanon = dstDir.absolutePath();
    if(dstCanon == srcCanon || dstCanon.startsWith(srcCanon + "/"))
        return;
    if(QFileInfo::exists(dstFi.absoluteFilePath()) && !dstFi.isDir()) { // overwriting file with a folder
        if(!overwriteFiles && !overwriteFiles.all) {
            overwriteFiles =
                ui_->fileReplaceDialog(srcFi.absoluteFilePath(), dstFi.absoluteFilePath(), DIR_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            if(!overwriteFiles.all) // reset temp flag right away
                overwriteFiles.yes = false;
        }
        // remove dst file; give up if not writable
        FileOpResult result;
        FileOperations::removeFile(dstFi.absoluteFilePath(), result);
        if(result != FileOpResult::SUCCESS) {
            overlays_->showError(FileOperations::decodeResult(result));
            qDebug() << FileOperations::decodeResult(result);
            return;
        }
    }
    // reached from both branches: a fresh destination needs creating, and a
    // destination that was just a removed FILE needs creating too — the
    // else-if here used to skip it, so "yes" on a dir-over-file collision
    // deleted the file and then copied nothing into a directory that never
    // existed
    if(!dstDir.mkpath(".")) {
        overlays_->showError(tr("Could not create directory ") + dstDir.absolutePath());
        qDebug() << "Could not create directory " << dstDir.absolutePath();
        return;
    }
    // copy all contents
    // TODO: skip symlinks? test
    QStringList entryList =
        srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for(const auto &entry : std::as_const(entryList)) {
        doInteractiveCopy(srcDir.absolutePath() + "/" + entry, dstDir.absolutePath(), overwriteFiles);
        if(overwriteFiles.cancel)
            return;
    }
}

void Core::interactiveMove(const QList<QString> &paths, const QString &destDirectory) {
    DialogResult overwriteFiles;
    for(const auto &path : paths) {
        doInteractiveMove(path, destDirectory, overwriteFiles);
        if(overwriteFiles.cancel)
            return;
    }
}

// todo: replacing DIR with a FILE?
void Core::doInteractiveMove(const QString &path, const QString &destDirectory, DialogResult &overwriteFiles) {
    QFileInfo srcFi(path);
    // SINGLE FILE MOVE ===========================================================================
    if(!srcFi.isDir()) {
        FileOpResult result;
        model->moveFileTo(path, destDirectory, overwriteFiles, result);
        if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
            if(overwriteFiles.all) // skipping all
                return;
            overwriteFiles = ui_->fileReplaceDialog(srcFi.absoluteFilePath(), destDirectory + "/" + srcFi.fileName(),
                                                    FILE_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            model->moveFileTo(path, destDirectory, true, result);
        }
        if(result == FileOpResult::SOURCE_NOT_REMOVED) {
            // the destination has the file; only the source removal failed.
            // A warning, not an error: reporting it red would contradict the
            // file the user can see at the destination
            overlays_->showWarning(FileOperations::decodeResult(result));
        } else if(result != FileOpResult::SUCCESS &&
                  !(result == FileOpResult::DESTINATION_FILE_EXISTS && !overwriteFiles)) {
            overlays_->showError(FileOperations::decodeResult(result));
            qDebug() << FileOperations::decodeResult(result);
        }
        if(!overwriteFiles.all) // move attempt done; reset temporary flag
            overwriteFiles.yes = false;
        return;
    }
    // DIR MOVE (RECURSIVE) =======================================================================
    QDir srcDir(srcFi.absoluteFilePath());
    // same two fixes as the dir copy above: whole name, no self-subtree
    QFileInfo dstFi(destDirectory + "/" + srcFi.fileName());
    QDir dstDir(dstFi.absoluteFilePath());
    QString srcCanon = srcDir.canonicalPath();
    QString dstCanon = dstDir.canonicalPath();
    if(dstCanon.isEmpty())
        dstCanon = dstDir.absolutePath();
    if(dstCanon == srcCanon || dstCanon.startsWith(srcCanon + "/"))
        return;
    if(QFileInfo::exists(dstFi.absoluteFilePath()) && !dstFi.isDir()) { // overwriting file with a folder
        if(!overwriteFiles && !overwriteFiles.all) {
            overwriteFiles =
                ui_->fileReplaceDialog(srcFi.absoluteFilePath(), dstFi.absoluteFilePath(), DIR_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            if(!overwriteFiles.all) // reset temp flag right away
                overwriteFiles.yes = false;
        }
        // remove dst file; give up if not writable
        FileOpResult result;
        FileOperations::removeFile(dstFi.absoluteFilePath(), result);
        if(result != FileOpResult::SUCCESS) {
            overlays_->showError(FileOperations::decodeResult(result));
            qDebug() << FileOperations::decodeResult(result);
            return;
        }
    }
    // reached from both branches: a fresh destination needs creating, and a
    // destination that was just a removed FILE needs creating too — the
    // else-if here used to skip it, so "yes" on a dir-over-file collision
    // deleted the file and then copied nothing into a directory that never
    // existed
    if(!dstDir.mkpath(".")) {
        overlays_->showError(tr("Could not create directory ") + dstDir.absolutePath());
        qDebug() << "Could not create directory " << dstDir.absolutePath();
        return;
    }
    // move all contents
    // TODO: skip symlinks? test
    QStringList entryList =
        srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for(const auto &entry : std::as_const(entryList)) {
        doInteractiveMove(srcDir.absolutePath() + "/" + entry, dstDir.absolutePath(), overwriteFiles);
        if(overwriteFiles.cancel)
            return;
    }
    FileOpResult dirRmRes;
    model->removeDir(srcDir.absolutePath(), false, false, dirRmRes);
}

void Core::copyPathsTo(const QList<QString> &paths, const QString &destDirectory) {
    interactiveCopy(paths, destDirectory);
}

void Core::movePathsTo(const QList<QString> &paths, const QString &destDirectory) {
    interactiveMove(paths, destDirectory);
}

void Core::moveCurrentFile(const QString &destDirectory) {
    if(model->isEmpty())
        return;
    // pause updates to avoid flicker
    view_->setUpdatesEnabled(false);
    // move fails during file playback, so we close it temporarily
    view_->closeImage();
    FileOpResult result;
    model->moveFileTo(selectedPath(), destDirectory, false, result);
    if(result == FileOpResult::SUCCESS) {
        overlays_->showMessageSuccess(tr("File moved."));
    } else if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
        if(ui_->showConfirmation(tr("File exists"), tr("Destination file exists. Overwrite?")))
            model->moveFileTo(selectedPath(), destDirectory, true, result);
    }
    if(result != FileOpResult::SUCCESS) {
        guiSetImage(model->getImage(selectedPath()));
        updateInfoString();
        if(result != FileOpResult::DESTINATION_FILE_EXISTS) {
            // a move whose copy landed but whose source could not be removed is
            // a warning with an accurate message, not a plain error
            if(result == FileOpResult::SOURCE_NOT_REMOVED)
                overlays_->showWarning(FileOperations::decodeResult(result));
            else
                outputError(result);
        }
    }
    view_->setUpdatesEnabled(true);
    view_->repaintView();
}

void Core::copyCurrentFile(const QString &destDirectory) {
    if(model->isEmpty())
        return;
    FileOpResult result;
    model->copyFileTo(selectedPath(), destDirectory, false, result);
    if(result == FileOpResult::SUCCESS) {
        overlays_->showMessageSuccess(tr("File copied."));
    } else if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
        if(ui_->showConfirmation(tr("File exists"), tr("Destination file exists. Overwrite?")))
            model->copyFileTo(selectedPath(), destDirectory, true, result);
    }
    if(result != FileOpResult::SUCCESS && result != FileOpResult::DESTINATION_FILE_EXISTS)
        outputError(result);
}

void Core::showRenameDialog() {
    if(model->isEmpty())
        return;
    QFileInfo fi(selectedPath());
    overlays_->toggleRenameOverlay(fi.fileName());
}
