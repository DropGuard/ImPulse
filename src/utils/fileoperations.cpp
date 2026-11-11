#include "fileoperations.h"

#include <QCoreApplication>
#include <QImage>
#include <QSaveFile>

#include <atomic>
#include <cstdio> // ::rename — the atomic step in rename()

QString FileOperations::generateHash(const QString &str) {
    return QString(QCryptographicHash::hash(str.toUtf8(), QCryptographicHash::Md5).toHex());
}

// A sibling path that belongs to THIS process. Used for the temporary a
// forced overwrite moves the old file to, before the new one is written under
// the real name.
//
// The pid is the point. The hash alone is a pure function of the destination,
// so two processes overwriting the same file used the same path: one removed
// the other's backup, and whichever lost that race could restore the wrong
// contents — or find its restore already consumed. A counter separates two
// operations inside one process, which is what a batch copy of the same file
// name into two directories can otherwise reduce to if the path is reused
// quickly.
QString siblingTempPath(const QString &destPath) {
    static std::atomic<quint64> seq{0};
    return destPath + QStringLiteral(".impulse-tmp-") + QString::number(QCoreApplication::applicationPid()) +
           QLatin1Char('-') + QString::number(seq.fetch_add(1, std::memory_order_relaxed));
}

void FileOperations::removeFile(const QString &filePath, FileOpResult &result) {
    QFileInfo file(filePath);
    if(!file.exists()) {
        result = FileOpResult::SOURCE_DOES_NOT_EXIST;
#ifdef Q_OS_WIN32
    } else if(!file.isWritable()) {
        result = FileOpResult::SOURCE_NOT_WRITABLE;
#endif
    } else {
        if(QFile::remove(filePath))
            result = FileOpResult::SUCCESS;
        else
            result = FileOpResult::OTHER_ERROR;
    }
    return;
}

// non-recursive
void FileOperations::removeDir(const QString &dirPath, bool recursive, FileOpResult &result) {
    QDir dir(dirPath);
    if(!dir.exists()) {
        result = FileOpResult::SOURCE_DOES_NOT_EXIST;
    } else {
        if(recursive ? dir.removeRecursively() : dir.rmdir(dirPath))
            result = FileOpResult::SUCCESS;
        else if(!recursive && !dir.isEmpty())
            result = FileOpResult::DIRECTORY_NOT_EMPTY;
        else
            result = FileOpResult::OTHER_ERROR;
    }
    return;
}

QString FileOperations::decodeResult(const FileOpResult &result) {
    switch(result) {
    case FileOpResult::SUCCESS: return QObject::tr("Operation completed succesfully.");
    case FileOpResult::DESTINATION_FILE_EXISTS: return QObject::tr("Destination file exists.");
    case FileOpResult::DESTINATION_DIR_EXISTS: return QObject::tr("Destination directory exists.");
    case FileOpResult::SOURCE_NOT_WRITABLE: return QObject::tr("Source file is not writable.");
    case FileOpResult::DESTINATION_NOT_WRITABLE: return QObject::tr("Destination is not writable.");
    case FileOpResult::SOURCE_DOES_NOT_EXIST: return QObject::tr("Source file does not exist.");
    case FileOpResult::DESTINATION_DOES_NOT_EXIST: return QObject::tr("Destination does not exist.");
    case FileOpResult::DIRECTORY_NOT_EMPTY: return QObject::tr("Directory is not empty.");
    case FileOpResult::SOURCE_NOT_REMOVED: return QObject::tr("Copied, but the original could not be removed.");
    case FileOpResult::NOTHING_TO_DO: return QObject::tr("Nothing to do.");
    case FileOpResult::OTHER_ERROR: return QObject::tr("Other error.");
    }
    return nullptr;
}

void FileOperations::copyFileTo(const QString &srcFilePath, const QString &destDirPath, bool force,
                                FileOpResult &result) {
    QFileInfo srcFile(srcFilePath);
    QString tmpPath;
    bool exists = false;
    // error checks
    if(destDirPath == srcFile.absolutePath()) {
        result = FileOpResult::NOTHING_TO_DO;
        return;
    }
    if(!srcFile.exists()) {
        result = FileOpResult::SOURCE_DOES_NOT_EXIST;
        return;
    }
    QFileInfo destDir(destDirPath);
    if(!destDir.exists()) {
        result = FileOpResult::DESTINATION_DOES_NOT_EXIST;
        return;
    }
    if(!destDir.isWritable()) {
        result = FileOpResult::DESTINATION_NOT_WRITABLE;
        return;
    }
    QFileInfo destFile(destDirPath + "/" + srcFile.fileName());
    if(destFile.exists()) {
#ifdef Q_OS_WIN32
        if(!destFile.isWritable()) {
            result = FileOpResult::DESTINATION_NOT_WRITABLE;
            return;
        }
#endif
        if(destFile.isDir()) {
            result = FileOpResult::DESTINATION_DIR_EXISTS;
            return;
        }
        if(!force) {
            result = FileOpResult::DESTINATION_FILE_EXISTS;
            return;
        }
        tmpPath = siblingTempPath(destFile.absoluteFilePath());
        // move backup — and only claim a backup exists if the move happened
        if(!QFile::rename(destFile.absoluteFilePath(), tmpPath)) {
            result = FileOpResult::OTHER_ERROR;
            return;
        }
        exists = true;
    }
    // copy
    auto srcModTime = srcFile.lastModified();
    auto srcReadTime = srcFile.lastRead();
    if(QFile::copy(srcFile.absoluteFilePath(), destFile.absoluteFilePath())) {
        result = FileOpResult::SUCCESS;
        // restore timestamps (best effort: the copy itself already succeeded)
        QFile dstF(destFile.absoluteFilePath());
        if(dstF.open(QIODevice::ReadWrite)) {
            dstF.setFileTime(srcModTime, QFileDevice::FileModificationTime);
            dstF.setFileTime(srcReadTime, QFileDevice::FileAccessTime);
            dstF.close();
        } else {
            qWarning() << "[FileOperations] timestamp restore failed:" << dstF.errorString();
        }
        // ok; remove the backup
        if(exists)
            QFile::remove(tmpPath);
    } else {
        result = FileOpResult::OTHER_ERROR;
        // fail; revert — a revert that itself fails means the previous file only
        // exists as the backup, so say where it is instead of losing the trail
        if(exists && !QFile::rename(tmpPath, destFile.absoluteFilePath()))
            qCritical() << "[FileOperations] copy failed and the backup could not be"
                        << "restored to" << destFile.absoluteFilePath() << "— it is still in" << tmpPath;
    }
    return;
}

void FileOperations::moveFileTo(const QString &srcFilePath, const QString &destDirPath, bool force,
                                FileOpResult &result) {
    QFileInfo srcFile(srcFilePath);
    QString tmpPath;
    bool exists = false;
    // error checks
    if(destDirPath == srcFile.absolutePath()) {
        result = FileOpResult::NOTHING_TO_DO;
        return;
    }
    if(!srcFile.exists()) {
        result = FileOpResult::SOURCE_DOES_NOT_EXIST;
        return;
    }
#ifdef Q_OS_WIN32
    if(!srcFile.isWritable()) {
        result = FileOpResult::SOURCE_NOT_WRITABLE;
        return;
    }
#endif
    QFileInfo destDir(destDirPath);
    if(!destDir.exists()) {
        result = FileOpResult::DESTINATION_DOES_NOT_EXIST;
        return;
    }
    if(!destDir.isWritable()) {
        result = FileOpResult::DESTINATION_NOT_WRITABLE;
        return;
    }
    QFileInfo destFile(destDirPath + "/" + srcFile.fileName());
    if(destFile.exists()) {
#ifdef Q_OS_WIN32
        if(!destFile.isWritable()) {
            result = FileOpResult::DESTINATION_NOT_WRITABLE;
            return;
        }
#endif
        if(destFile.isDir()) {
            result = FileOpResult::DESTINATION_DIR_EXISTS;
            return;
        }
        if(!force) {
            result = FileOpResult::DESTINATION_FILE_EXISTS;
            return;
        }
        tmpPath = siblingTempPath(destFile.absoluteFilePath());
        // move backup
        if(!QFile::rename(destFile.absoluteFilePath(), tmpPath)) {
            result = FileOpResult::OTHER_ERROR;
            return;
        }
        exists = true;
    }
    // move
    auto srcModTime = srcFile.lastModified();
    auto srcReadTime = srcFile.lastRead();
    if(QFile::copy(srcFile.absoluteFilePath(), destFile.absoluteFilePath())) {
        // The destination now holds the file. Whether the SOURCE can be removed
        // is a separate fact, and it is reported as one: the move is done at
        // the destination either way, and the old contract — undo the copy,
        // put the old destination back — threw away work the user can see
        // succeeded and answered with a generic error. A user told "moved to
        // X, but the original could not be deleted" knows exactly what is left
        // to do; a user told "Other error" after the file visibly appeared at
        // the destination knows nothing.
        //
        // The backup is only restored when the COPY failed, which is the case
        // where the destination never changed.
        FileOpResult removeResult;
        removeFile(srcFile.absoluteFilePath(), removeResult);
        if(removeResult == FileOpResult::SUCCESS) {
            result = FileOpResult::SUCCESS;
            // restore timestamps
            QFile dstF(destFile.absoluteFilePath());
            if(dstF.open(QIODevice::ReadWrite)) {
                dstF.setFileTime(srcModTime, QFileDevice::FileModificationTime);
                dstF.setFileTime(srcReadTime, QFileDevice::FileAccessTime);
                dstF.close();
            } else {
                qWarning() << "[FileOperations] timestamp restore failed:" << dstF.errorString();
            }
            // remove backup
            if(exists)
                QFile::remove(tmpPath);
            return;
        }
        // the copy stands; only the source removal failed
        result = FileOpResult::SOURCE_NOT_REMOVED;
        if(exists)
            QFile::remove(tmpPath);
        return;
    }
    // could not COPY: the destination is untouched, so the backup goes back
    result = FileOpResult::OTHER_ERROR;
    if(exists && !QFile::rename(tmpPath, destFile.absoluteFilePath()))
        qCritical() << "[FileOperations] copy failed and the backup could not be"
                    << "restored to" << destFile.absoluteFilePath() << "— it is still in" << tmpPath;
}

void FileOperations::rename(const QString &srcFilePath, const QString &newName, bool force, FileOpResult &result) {
    QFileInfo srcFile(srcFilePath);
    // error checks
    if(!srcFile.exists()) {
        result = FileOpResult::SOURCE_DOES_NOT_EXIST;
        return;
    }
#ifdef Q_OS_WIN32
    if(!srcFile.isWritable()) {
        result = FileOpResult::SOURCE_NOT_WRITABLE;
        return;
    }
#endif
    if(newName.isEmpty() || newName == srcFile.fileName()) {
        result = FileOpResult::NOTHING_TO_DO;
        return;
    }
    QString newFilePath = srcFile.absolutePath() + "/" + newName;
    QFileInfo destFile(newFilePath);
    if(destFile.exists()) {
#ifdef Q_OS_WIN32
        if(!destFile.isWritable())
            result = FileOpResult::DESTINATION_NOT_WRITABLE;
#endif
        if(destFile.isDir()) {
            result = FileOpResult::DESTINATION_DIR_EXISTS;
            return;
        }
        if(!force) {
            result = FileOpResult::DESTINATION_FILE_EXISTS;
            return;
        }
    }
    // One rename(2), which replaces an existing destination atomically. The
    // previous shape moved the destination aside under a fixed
    // "<name>_<md5>" backup, renamed the source on top, then deleted the
    // backup — three syscalls where one does, and a backup file that a
    // concurrent process could delete (the name carried no pid) and that a
    // SIGKILL could leave on disk forever.
    //
    // ::rename rather than QFile::rename because Qt's is RENAME_NOREPLACE:
    // it answers EEXIST when the destination exists, which is exactly the
    // forced-overwrite case this branch is in. The bytes of the name are
    // UTF-8 for the same reason saveWithBackup's rollback used them — that is
    // what the listing keys on.
    const QByteArray from = srcFile.filePath().toUtf8();
    const QByteArray to = newFilePath.toUtf8();
    result = ::rename(from.constData(), to.constData()) == 0 ? FileOpResult::SUCCESS : FileOpResult::OTHER_ERROR;
}

void FileOperations::moveToTrash(const QString &filePath, FileOpResult &result) {
    QFileInfo file(filePath);
    if(!file.exists()) {
        result = FileOpResult::SOURCE_DOES_NOT_EXIST;
#ifdef Q_OS_WIN32
    } else if(!file.isWritable()) {
        result = FileOpResult::SOURCE_NOT_WRITABLE;
#endif
    } else {
        if(moveToTrashImpl(filePath))
            result = FileOpResult::SUCCESS;
        else
            result = FileOpResult::OTHER_ERROR;
    }
    return;
}

bool FileOperations::moveToTrashImpl(const QString &filePath) {
    return QFile::moveToTrash(filePath);
}

// One question, one place: the encoder for this suffix, and the quality it
// deserves. png gets libpng's flat 30 (levels 3-6 perform as well as 9 here),
// jpeg gets the user's setting, anything else is left to the default and will
// simply fail to encode.
static bool writeImageTo(const QImage &image, const QString &destPath, int jpegQuality) {
    const QString ext = QFileInfo(destPath).suffix();
    int quality = 95;
    if(ext.compare("png", Qt::CaseInsensitive) == 0)
        quality = 30;
    else if(ext.compare("jpg", Qt::CaseInsensitive) == 0 || ext.compare("jpeg", Qt::CaseInsensitive) == 0)
        quality = jpegQuality;

    // QSaveFile writes a uniquely named temporary in the SAME directory and
    // rename(2)s it into place on commit, so the old, hand-rolled
    // "copy the original aside, write over it, rename the copy back on failure"
    // is gone. That scheme had three defects this replaces by construction:
    // the backup's name was a fixed md5 of the path (two processes overwriting
    // the same file deleted each other's backup), a SIGKILL left the backup on
    // disk forever, and a failed write was undone by a second filesystem
    // operation that could itself fail.
    //
    // The guarantees now: the destination holds either its previous contents or
    // the complete new image, never a partial write, and a failure leaves no
    // residue. QSaveFile preserves an existing file's permissions on commit
    // (measured), so chmod through a save is not a behaviour change.
    QSaveFile out(destPath);
    if(!out.open(QIODevice::WriteOnly))
        return false;
    if(!image.save(&out, ext.toStdString().c_str(), quality)) {
        // cancelWriting(), not just returning: the temporary must go, and on
        // this path it would otherwise be removed only by the destructor — the
        // same effect, stated once where a reader looks for it
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

bool FileOperations::saveWithBackup(const QImage &image, const QString &destPath, int jpegQuality) {
    return writeImageTo(image, destPath, jpegQuality);
}
