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

#ifdef _WIN32
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
    if(!m_ui->showConfirmation(tr("Delete permanently"), msg))
        return;
    FileOpResult result;
    int successCount = 0;
    for(const auto &path : paths) {
        QFileInfo fi(path);
        if(fi.isDir())
            m_model->removeDir(path, false, true, result);
        else
            result = removeFile(path, false);
        if(result == FileOpResult::SUCCESS)
            successCount++;
        else
            qDebug() << "remove failed:" << path << FileOperations::decodeResult(result);
    }
    if(paths.count() == 1) {
        if(result == FileOpResult::SUCCESS)
            m_overlays->showMessageSuccess(tr("File removed"));
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
        if(!m_ui->showConfirmation(tr("Move to trash"), msg))
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
            m_overlays->showMessageSuccess(tr("Moved to trash"));
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
    if(m_model->listing().isEmpty())
        return;
    m_model->reload(filePath);
}

void Core::renameCurrentSelection(const QString &newName) {
    // Nothing is armed here: the watcher pairs the move into one Renamed event,
    // and Core::onFileRenamed follows it for the file on screen — an in-app
    // rename and an external one take the same path and get the same result.
    const QString oldPath = selectedPath();
    if(!m_model->listing().fileCount() || newName.isEmpty() || oldPath.isEmpty())
        return;
    FileOpResult result;
    m_model->renameEntry(oldPath, newName, false, result);
    if(result == FileOpResult::DESTINATION_DIR_EXISTS) {
        m_overlays->toggleRenameOverlay(newName);
    } else if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
        if(m_ui->showConfirmation(tr("File exists"), tr("Overwrite file?"))) {
            m_model->renameEntry(oldPath, newName, true, result);
        } else {
            // show rename dialog again
            m_overlays->toggleRenameOverlay(newName);
        }
    }
    outputError(result);
}

FileOpResult Core::removeFile(const QString &filePath, bool trash) {
    if(m_model->listing().isEmpty())
        return FileOpResult::NOTHING_TO_DO;

    bool reopen = false;
    std::shared_ptr<Image> img;
    if(m_state.currentFilePath == filePath) {
        img = m_model->getImage(filePath);
        if(img->type() == ANIMATED || img->type() == VIDEO) {
            m_view->closeImage();
            reopen = true;
        }
    }
    FileOpResult result;
    m_model->removeFile(filePath, trash, result);
    if(result != FileOpResult::SUCCESS && reopen)
        guiSetImage(img);
    return result;
}

void Core::outputError(const FileOpResult &error) const {
    if(error == FileOpResult::SUCCESS || error == FileOpResult::NOTHING_TO_DO)
        return;
    m_overlays->showError(FileOperations::decodeResult(error));
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
        m_overlays->showMessageSuccess(doneVerb + QString::number(successCount) + tr(" files"));
    else
        m_overlays->showError(doneVerb + QString::number(successCount) + tr(" files, ") + QString::number(failedCount) +
                              tr(" failed"));
}

void Core::showOpenDialog() {
    m_dialogs->showOpenDialog(m_model->listing().directoryPath());
}

void Core::showInDirectory() {
    if(!m_model)
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
    QString reveal = selectedPath().isEmpty() ? m_model->listing().directoryPath() : selectedPath();
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
            m_overlays->showMessage(tr("No file manager is available to reveal the file"));
    });
#elif defined(_WIN32)
    // _WIN32, not __WIN32: the latter is not a macro any compiler defines, so
    // this branch never compiled and every platform fell through to the
    // QDesktopServices fallback below.
    QStringList args;
    args << "/select," << QDir::toNativeSeparators(selectedPath());
    QProcess::startDetached("explorer", args);
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_model->listing().directoryPath()));
#endif
}

// upstream issue #636, promoted from the author's own external script: one
// keypress copies the current file into the configured folder — no dialog,
// no clobbering. Unconfigured, it says so instead of guessing.
void Core::quickCopyToFixedDirectory() {
    if(!m_model || m_model->listing().isEmpty() || selectedPath().isEmpty())
        return;
    QString destDir = settings->quickCopyDirectory();
    if(destDir.isEmpty()) {
        m_overlays->showMessage(tr("Set the quick copy folder in settings first"));
        return;
    }
    QFileInfo srcFi(selectedPath());
    if(!srcFi.isFile()) {
        m_overlays->showMessage(tr("Quick copy: not a regular file"));
        return;
    }
    QDir(destDir).mkpath(".");
    if(QFileInfo::exists(destDir + "/" + srcFi.fileName())) {
        m_overlays->showMessage(tr("Already in the quick copy folder"));
        return;
    }
    FileOpResult result;
    FileOperations::copyFileTo(selectedPath(), destDir, false, result);
    if(result == FileOpResult::SUCCESS)
        m_overlays->showMessage(tr("Copied to ") + destDir);
    else
        m_overlays->showError(FileOperations::decodeResult(result));
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
            overwriteFiles = m_ui->fileReplaceDialog(srcFi.absoluteFilePath(), destDirectory + "/" + srcFi.fileName(),
                                                     FILE_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            FileOperations::copyFileTo(path, destDirectory, true, result);
        }
        if(result != FileOpResult::SUCCESS && !(result == FileOpResult::DESTINATION_FILE_EXISTS && !overwriteFiles)) {
            m_overlays->showError(FileOperations::decodeResult(result));
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
                m_ui->fileReplaceDialog(srcFi.absoluteFilePath(), dstFi.absoluteFilePath(), DIR_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            if(!overwriteFiles.all) // reset temp flag right away
                overwriteFiles.yes = false;
        }
        // remove dst file; give up if not writable
        FileOpResult result;
        FileOperations::removeFile(dstFi.absoluteFilePath(), result);
        if(result != FileOpResult::SUCCESS) {
            m_overlays->showError(FileOperations::decodeResult(result));
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
        m_overlays->showError(tr("Could not create directory ") + dstDir.absolutePath());
        qDebug() << "Could not create directory " << dstDir.absolutePath();
        return;
    }
    // copy all contents. A symlink to a directory is NOT descended into:
    // QFileInfo::isDir() answers true for one, so the recursion below would
    // copy the target's contents — and a link pointing back up the tree
    // recurses until the path stops resolving. Measured before the guard: a
    // two-directory fixture with one link between them never terminated.
    const QStringList entryList =
        srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for(const auto &entry : std::as_const(entryList)) {
        const QString entryPath = srcDir.absolutePath() + "/" + entry;
        if(QFileInfo(entryPath).isSymLink()) {
            // Copy the LINK, not what it points at: QFile::copy follows the
            // link and would duplicate the target's contents (measured). A
            // target the reader refuses to name (or a relative one) is
            // re-anchored to the absolute resolved path, so the recreation
            // lands correctly wherever the destination sits.
            QString linkTarget = QFileInfo(entryPath).symLinkTarget();
            if(linkTarget.isEmpty())
                linkTarget = QFileInfo(entryPath).canonicalFilePath();
            const QString linkDest = dstDir.absolutePath() + "/" + entry;
            QFile::remove(linkDest);
            FileOperations::createSymlink(linkTarget, linkDest);
            continue;
        }
        doInteractiveCopy(entryPath, dstDir.absolutePath(), overwriteFiles);
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
        m_model->moveFileTo(path, destDirectory, overwriteFiles, result);
        if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
            if(overwriteFiles.all) // skipping all
                return;
            overwriteFiles = m_ui->fileReplaceDialog(srcFi.absoluteFilePath(), destDirectory + "/" + srcFi.fileName(),
                                                     FILE_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            m_model->moveFileTo(path, destDirectory, true, result);
        }
        if(result == FileOpResult::SOURCE_NOT_REMOVED) {
            // the destination has the file; only the source removal failed.
            // A warning, not an error: reporting it red would contradict the
            // file the user can see at the destination
            m_overlays->showWarning(FileOperations::decodeResult(result));
        } else if(result != FileOpResult::SUCCESS &&
                  !(result == FileOpResult::DESTINATION_FILE_EXISTS && !overwriteFiles)) {
            m_overlays->showError(FileOperations::decodeResult(result));
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
                m_ui->fileReplaceDialog(srcFi.absoluteFilePath(), dstFi.absoluteFilePath(), DIR_TO_FILE, true);
            if(!overwriteFiles || overwriteFiles.cancel)
                return;
            if(!overwriteFiles.all) // reset temp flag right away
                overwriteFiles.yes = false;
        }
        // remove dst file; give up if not writable
        FileOpResult result;
        FileOperations::removeFile(dstFi.absoluteFilePath(), result);
        if(result != FileOpResult::SUCCESS) {
            m_overlays->showError(FileOperations::decodeResult(result));
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
        m_overlays->showError(tr("Could not create directory ") + dstDir.absolutePath());
        qDebug() << "Could not create directory " << dstDir.absolutePath();
        return;
    }
    // move all contents. Same guard as the copy: a symlink to a directory
    // reports isDir() true and would be descended into.
    const QStringList entryList =
        srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for(const auto &entry : std::as_const(entryList)) {
        const QString entryPath = srcDir.absolutePath() + "/" + entry;
        if(QFileInfo(entryPath).isSymLink()) {
            // Move the LINK, not its target: QFile::copy (which moveFileTo
            // uses) follows it. Recreate the same link at the destination and
            // drop the original; a failure here is reported like any other.
            // Same re-anchoring as the copy: a relative or unnamed target
            // must not recreate the link against the wrong directory.
            QString linkTarget = QFileInfo(entryPath).symLinkTarget();
            if(linkTarget.isEmpty())
                linkTarget = QFileInfo(entryPath).canonicalFilePath();
            const QString linkDest = dstDir.absolutePath() + "/" + entry;
            QFile::remove(linkDest);
            if(FileOperations::createSymlink(linkTarget, linkDest)) {
                // The link goes with the move, and only the link: removeSymlink
                // unlinks the reparse point itself and never the target. The two
                // alternatives a reader might reach for both fail — QDir::rmdir
                // refuses a symlink outright, and removeRecursively follows it
                // and empties the target first (both measured on Linux).
                FileOperations::removeSymlink(entryPath);
            }
            continue;
        }
        doInteractiveMove(entryPath, dstDir.absolutePath(), overwriteFiles);
        if(overwriteFiles.cancel)
            return;
    }
    FileOpResult dirRmRes;
    m_model->removeDir(srcDir.absolutePath(), false, false, dirRmRes);
}

void Core::copyPathsTo(const QList<QString> &paths, const QString &destDirectory) {
    interactiveCopy(paths, destDirectory);
}

void Core::movePathsTo(const QList<QString> &paths, const QString &destDirectory) {
    interactiveMove(paths, destDirectory);
}

void Core::moveCurrentFile(const QString &destDirectory) {
    if(m_model->listing().isEmpty())
        return;
    // pause updates to avoid flicker
    m_view->setUpdatesEnabled(false);
    // move fails during file playback, so we close it temporarily
    m_view->closeImage();
    FileOpResult result;
    m_model->moveFileTo(selectedPath(), destDirectory, false, result);
    if(result == FileOpResult::SUCCESS) {
        m_overlays->showMessageSuccess(tr("File moved."));
    } else if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
        if(m_ui->showConfirmation(tr("File exists"), tr("Destination file exists. Overwrite?")))
            m_model->moveFileTo(selectedPath(), destDirectory, true, result);
    }
    if(result != FileOpResult::SUCCESS) {
        guiSetImage(m_model->getImage(selectedPath()));
        updateInfoString();
        if(result != FileOpResult::DESTINATION_FILE_EXISTS) {
            // a move whose copy landed but whose source could not be removed is
            // a warning with an accurate message, not a plain error
            if(result == FileOpResult::SOURCE_NOT_REMOVED)
                m_overlays->showWarning(FileOperations::decodeResult(result));
            else
                outputError(result);
        }
    }
    m_view->setUpdatesEnabled(true);
    m_view->repaintView();
}

void Core::copyCurrentFile(const QString &destDirectory) {
    if(m_model->listing().isEmpty())
        return;
    FileOpResult result;
    m_model->copyFileTo(selectedPath(), destDirectory, false, result);
    if(result == FileOpResult::SUCCESS) {
        m_overlays->showMessageSuccess(tr("File copied."));
    } else if(result == FileOpResult::DESTINATION_FILE_EXISTS) {
        if(m_ui->showConfirmation(tr("File exists"), tr("Destination file exists. Overwrite?")))
            m_model->copyFileTo(selectedPath(), destDirectory, true, result);
    }
    if(result != FileOpResult::SUCCESS && result != FileOpResult::DESTINATION_FILE_EXISTS)
        outputError(result);
}

void Core::showRenameDialog() {
    if(m_model->listing().isEmpty())
        return;
    QFileInfo fi(selectedPath());
    m_overlays->toggleRenameOverlay(fi.fileName());
}
