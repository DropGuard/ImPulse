#include "documentinfo.h"

DocumentInfo::DocumentInfo(QString path, const LoadPolicy &policy)
    : m_documentType(DocumentType::NONE), m_orientation(0), m_format(""), m_exifLoaded(false) {
    m_fileInfo.setFile(path);
    if(!m_fileInfo.isFile()) {
        qDebug() << "FileInfo: cannot open: " << path;
        return;
    }
    detectFormat(policy);
}

DocumentInfo::~DocumentInfo() {
}

// ##############################################################
// ####################### PUBLIC METHODS #######################
// ##############################################################

QString DocumentInfo::directoryPath() const {
    return m_fileInfo.absolutePath();
}

QString DocumentInfo::filePath() const {
    return m_fileInfo.absoluteFilePath();
}

QString DocumentInfo::fileName() const {
    return m_fileInfo.fileName();
}

QString DocumentInfo::baseName() const {
    return m_fileInfo.baseName();
}

// bytes
qint64 DocumentInfo::fileSize() const {
    return m_fileInfo.size();
}

DocumentType DocumentInfo::type() const {
    return m_documentType;
}

QMimeType DocumentInfo::mimeType() const {
    return m_mimeType;
}

const QString &DocumentInfo::format() const {
    return m_format;
}

QDateTime DocumentInfo::lastModified() const {
    return m_fileInfo.lastModified();
}

// For cases like orientation / even mimetype change we just reload
// Image from scratch, so don`t bother handling it here
void DocumentInfo::refresh() {
    m_fileInfo.refresh();
}

int DocumentInfo::exifOrientation() const {
    return m_orientation;
}

// ##############################################################
// ####################### PRIVATE METHODS ######################
// ##############################################################
void DocumentInfo::detectFormat(const LoadPolicy &policy) {
    if(m_documentType != DocumentType::NONE)
        return;
    QMimeDatabase mimeDb;
    m_mimeType = mimeDb.mimeTypeForFile(m_fileInfo.filePath(), QMimeDatabase::MatchContent);
    auto mimeName = m_mimeType.name().toUtf8();
    auto suffix = m_fileInfo.suffix().toLower().toUtf8();
    if(mimeName == "image/jpeg") {
        m_format = "jpg";
        m_documentType = DocumentType::STATIC;
    } else if(mimeName == "image/png") {
        if(QImageReader::supportedImageFormats().contains("apng") && detectAPNG()) {
            m_format = "apng";
            m_documentType = DocumentType::ANIMATED;
        } else {
            m_format = "png";
            m_documentType = DocumentType::STATIC;
        }
    } else if(mimeName == "image/gif") {
        m_format = "gif";
        m_documentType = DocumentType::ANIMATED;
    } else if(mimeName == "image/webp" || (mimeName == "audio/x-riff" && suffix == "webp")) {
        m_format = "webp";
        m_documentType = detectAnimatedWebP() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/jxl") {
        m_format = "jxl";
        m_documentType = detectAnimatedJxl() ? DocumentType::ANIMATED : DocumentType::STATIC;
        if(m_documentType == DocumentType::ANIMATED && !policy.jxlAnimation) {
            m_documentType = DocumentType::NONE;
            qDebug() << "animated jxl is off; skipping file";
        }
    } else if(mimeName == "image/avif") {
        m_format = "avif";
        m_documentType = detectAnimatedAvif() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/bmp") {
        m_format = "bmp";
        m_documentType = DocumentType::STATIC;
    } else if(policy.videoPlayback && policy.videoFormats.contains(mimeName)) {
        m_documentType = DocumentType::VIDEO;
        m_format = policy.videoFormats.value(mimeName);
    } else {
        // just try to open via suffix if all of the above fails
        m_format = suffix;
        if(m_format.compare("jfif", Qt::CaseInsensitive) == 0)
            m_format = "jpg";
        if(policy.videoPlayback && policy.videoFormats.values().contains(suffix))
            m_documentType = DocumentType::VIDEO;
        else
            m_documentType = DocumentType::STATIC;
    }
    loadExifOrientation();
}

inline
    // dumb apng detector
    bool DocumentInfo::detectAPNG() {
    QFile f(m_fileInfo.filePath());
    if(f.open(QFile::ReadOnly)) {
        QDataStream in(&f);
        const int len = 120;
        QByteArray qbuf("\0", len);
        if(in.readRawData(qbuf.data(), len) > 0) {
            return qbuf.contains("acTL");
        }
    }
    return false;
}

bool DocumentInfo::detectAnimatedWebP() {
    QFile f(m_fileInfo.filePath());
    bool result = false;
    if(f.open(QFile::ReadOnly)) {
        QDataStream in(&f);
        in.skipRawData(12);
        // a stack buffer, not malloc: the size is a compile-time constant, and
        // an unchecked allocation is a null dereference on the one path where
        // memory is already short
        char buf[5];
        buf[4] = '\0';
        in.readRawData(buf, 4);
        if(strcmp(buf, "VP8X") == 0) {
            in.skipRawData(4);
            char flags;
            in.readRawData(&flags, 1);
            if(flags & (1 << 1)) {
                result = true;
            }
        }
    }
    return result;
}

// TODO avoid creating multiple QImageReader instances
bool DocumentInfo::detectAnimatedJxl() {
    QImageReader r(m_fileInfo.filePath(), "jxl");
    return r.supportsAnimation();
}

bool DocumentInfo::detectAnimatedAvif() {
    QFile f(m_fileInfo.filePath());
    bool result = false;
    if(f.open(QFile::ReadOnly)) {
        QDataStream in(&f);
        in.skipRawData(4); // skip box size
        char buf[9];
        buf[8] = '\0';
        in.readRawData(buf, 8);
        if(strcmp(buf, "ftypavis") == 0) {
            result = true;
        }
    }
    return result;
}

void DocumentInfo::loadExifTags() {
    if(m_exifLoaded)
        return;
    m_exifLoaded = true;
    m_exifTags.clear();
#ifdef USE_EXIV2
    try {
        std::unique_ptr<Exiv2::Image> image;

        // exiv2 0.28 dropped the unicode-path overloads: every platform takes
        // the UTF-8 narrow string (wide paths are converted internally), which
        // is exactly QString::toStdString — the old wide-char helper would
        // find no matching open() here
        image = Exiv2::ImageFactory::open(m_fileInfo.filePath().toStdString());

        assert(image.get() != 0);
        image->readMetadata();
        Exiv2::ExifData &exifData = image->exifData();
        if(exifData.empty())
            return;

        Exiv2::ExifKey make("Exif.Image.Make");
        Exiv2::ExifKey model("Exif.Image.Model");
        Exiv2::ExifKey dateTime("Exif.Image.DateTime");
        Exiv2::ExifKey exposureTime("Exif.Photo.ExposureTime");
        Exiv2::ExifKey fnumber("Exif.Photo.FNumber");
        Exiv2::ExifKey isoSpeedRatings("Exif.Photo.ISOSpeedRatings");
        Exiv2::ExifKey flash("Exif.Photo.Flash");
        Exiv2::ExifKey focalLength("Exif.Photo.FocalLength");
        Exiv2::ExifKey userComment("Exif.Photo.UserComment");

        Exiv2::ExifData::const_iterator it;

        it = exifData.findKey(make);
        if(it != exifData.end() /* && it->count() */)
            m_exifTags.insert(QObject::tr("Make"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(model);
        if(it != exifData.end())
            m_exifTags.insert(QObject::tr("Model"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(dateTime);
        if(it != exifData.end())
            m_exifTags.insert(QObject::tr("Date/Time"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(exposureTime);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            if(r.first < r.second) {
                qreal exp = round(static_cast<qreal>(r.second) / r.first);
                m_exifTags.insert(QObject::tr("ExposureTime"), "1/" + QString::number(exp) + QObject::tr(" sec"));
            } else {
                qreal exp = round(static_cast<qreal>(r.first) / r.second);
                m_exifTags.insert(QObject::tr("ExposureTime"), QString::number(exp) + QObject::tr(" sec"));
            }
        }

        it = exifData.findKey(fnumber);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            qreal fn = static_cast<qreal>(r.first) / r.second;
            m_exifTags.insert(QObject::tr("F Number"), "f/" + QString::number(fn, 'g', 3));
        }

        it = exifData.findKey(isoSpeedRatings);
        if(it != exifData.end())
            m_exifTags.insert(QObject::tr("ISO Speed ratings"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(flash);
        if(it != exifData.end())
            m_exifTags.insert(QObject::tr("Flash"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(focalLength);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            qreal fn = static_cast<qreal>(r.first) / r.second;
            m_exifTags.insert(QObject::tr("Focal Length"), QString::number(fn, 'g', 3) + QObject::tr(" mm"));
        }

        it = exifData.findKey(userComment);
        if(it != exifData.end()) {
            // crop out 'charset=ascii' etc"
            auto comment = QString::fromStdString(it->value().toString());
            if(comment.startsWith("charset="))
                comment.remove(0, comment.indexOf(" ") + 1);
            m_exifTags.insert(QObject::tr("UserComment"), comment);
        }
    }

    // One handler for both Exiv2 eras. 0.27's BasicError<char> IS Error (a
    // typedef), so a narrower catch before this one is unreachable and GCC 13
    // rejects the pair under -Wexceptions; 0.28 removed the BasicError
    // template entirely, so the branch that used to guard it never compiled
    // anywhere. Both facts point at the same code: this handler alone.
    catch(Exiv2::Error &e) {
        qDebug() << "Caught Exiv2 exception:\n" << e.what() << "\n";
        return;
    }
#endif
}

QMap<QString, QString> DocumentInfo::getExifTags() {
    if(!m_exifLoaded)
        loadExifTags();
    return m_exifTags;
}

void DocumentInfo::loadExifOrientation() {
    if(m_documentType == DocumentType::VIDEO || m_documentType == DocumentType::NONE)
        return;

    QString path = filePath();
    QImageReader *reader = nullptr;
    if(!m_format.isEmpty())
        reader = new QImageReader(path, m_format.toStdString().c_str());
    else
        reader = new QImageReader(path);

    if(reader->canRead())
        m_orientation = static_cast<int>(reader->transformation());
    delete reader;
}
