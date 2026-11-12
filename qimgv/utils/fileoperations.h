#pragma once

#include <QDebug>
#include <QString>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QtGlobal>

class QImage;

#ifdef Q_OS_WIN32
#include "windows.h"
#endif

enum FileOpResult {
    SUCCESS,
    DESTINATION_FILE_EXISTS,
    DESTINATION_DIR_EXISTS,
    SOURCE_NOT_WRITABLE,
    DESTINATION_NOT_WRITABLE,
    SOURCE_DOES_NOT_EXIST,
    DESTINATION_DOES_NOT_EXIST,
    DIRECTORY_NOT_EMPTY,
    NOTHING_TO_DO, // todo: maybe just return SUCCESS?
    // A cross-directory move copies, then removes the source. The copy landed
    // and the source removal did not: the file exists at BOTH paths, and the
    // caller must say so rather than report a failure that contradicts what
    // the user can see at the destination.
    SOURCE_NOT_REMOVED,
    OTHER_ERROR
};

class FileOperations {
  public:
    static void copyFileTo(const QString &srcFilePath, const QString &destDirPath, bool force, FileOpResult &result);
    static void moveFileTo(const QString &srcFilePath, const QString &destDirPath, bool force, FileOpResult &result);
    static void rename(const QString &srcFilePath, const QString &newName, bool force, FileOpResult &result);
    static void removeFile(const QString &filePath, FileOpResult &result);
    static void removeDir(const QString &dirPath, bool recursive, FileOpResult &result);
    static void moveToTrash(const QString &filePath, FileOpResult &result);

    static QString decodeResult(const FileOpResult &result);

    // The desktop's "file (1).jpg" pattern, owned here: the first
    // non-existing numbered sibling of filePath — the Save As prefill for an
    // edited image, so the quick path writes a copy instead of silently
    // overwriting the original.
    static QString nextFreeVariantPath(const QString &filePath);

    // The one implementation of "overwrite a file from memory, and put the old
    // contents back if the write fails". Both ImageStatic::save() and the
    // clipboard import carried their own copy; the copies had already diverged,
    // and the clipboard one still held the rollback bug the other fixed - it
    // deleted the original, then deleted the backup even when the restore
    // itself failed, turning "save failed" into "your file is gone".
    //
    // `metadataSource`, when it names an existing file, is where the metadata
    // to carry over is read from: Qt writes pixels and nothing else (its
    // writers emit no EXIF, XMP or IPTC), so an edit that goes through here
    // would otherwise strip everything the camera recorded. Empty for a source
    // that has none — the clipboard import, a brand-new file.
    static bool saveWithBackup(const QImage &image, const QString &destPath, int jpegQuality,
                               const QString &metadataSource = QString());

  private:
    static bool moveToTrashImpl(const QString &path);
    // Copies EXIF, XMP and IPTC from `source` onto the file at `dest`, which
    // must already hold the written pixels. Returns false when the source has
    // no metadata to carry or the format cannot hold it; the pixels are
    // already saved either way, so a false here is never a failed save.
    static bool carryMetadata(const QString &source, const QString &dest, bool resetOrientation);
};
