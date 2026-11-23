#pragma once

// The application state Core owns and the views read: the State properties
// and the CurrentInfo projection, plus the pure title assembly over it.
// Its own header because its readers are not Core's clients: the window and
// the crop controller bind to State, and reaching it through core.h made
// every GUI file that renders state depend on the composition root — which
// itself includes the window, so the two headers depended on each other.
// Nothing here may include a component or a gui header.

#include <QLocale>
#include <QProperty>
#include <QSize>
#include <QString>

#include "settings.h" // ViewMode

// the projection Core maintains for the views: everything the title bar,
// info bars and overlays render is derived from this — views never store
// copies (upstream kept a mirror struct in MW that drifted from Core's
// state; the projection-in-state shape is what retires it)
struct CurrentInfo {
    int index = 0;
    int fileCount = 0;
    QString fileName;
    QString filePath;
    QString directoryName;
    QString directoryPath;
    QSize imageSize;
    qint64 fileSize = 0;
    bool slideshow = false;
    bool shuffle = false;
    bool edited = false;
};

// the title bar's string assembly, pure so tests can drive it without
// instantiating the window
inline QString titleSegments(const CurrentInfo &info, QString *posOut, QString *resOut, QString *sizeOut) {
    QString pos;
    if(info.fileCount)
        pos = "[ " + QString::number(info.index + 1) + "/" + QString::number(info.fileCount) + " ]";
    QString res;
    if(info.imageSize.width())
        res = QString::number(info.imageSize.width()) + " x " + QString::number(info.imageSize.height());
    QString size;
    if(info.fileSize)
        size = QLocale().formattedDataSize(info.fileSize, 1);
    if(posOut)
        *posOut = pos;
    if(resOut)
        *resOut = res;
    if(sizeOut)
        *sizeOut = size;
    return pos;
}

inline QString titleStateSuffix(bool slideshow, bool shuffle, bool zoomLock, bool viewLock) {
    QString states;
    if(slideshow)
        states.append(" [slideshow]");
    if(shuffle)
        states.append(" [shuffle]");
    if(zoomLock)
        states.append(" [zoom lock]");
    if(viewLock)
        states.append(" [view lock]");
    return states;
}

inline QString assembleWindowTitle(bool folderView, const CurrentInfo &info, bool zoomLock, bool viewLock,
                                   bool extendedInfo, bool appendStates, const QString &appName,
                                   const QString &folderViewTitle) {
    if(folderView)
        return folderViewTitle;
    if(info.fileName.isEmpty())
        return appName;
    QString pos, res, size;
    titleSegments(info, &pos, &res, &size);
    QString title = info.fileName;
    if(extendedInfo) {
        if(!pos.isEmpty())
            title.prepend(pos + "  ");
        if(!res.isEmpty())
            title.append("  -  " + res);
        if(!size.isEmpty())
            title.append("  -  " + size);
    }
    QString states = titleStateSuffix(info.slideshow, info.shuffle, zoomLock, viewLock);
    if(appendStates && !states.isEmpty())
        title.append(" -" + states);
    if(info.edited)
        title.prepend("* ");
    return title;
}

struct State {
    // scalar state lives in QProperties: bindable storage for the pull-style
    // views (P2 of the unidirectional-flow refactor), plain operator= writes
    QProperty<bool> hasActiveImage{false};
    // the view mode is Core's decision, recorded here; the window subscribes
    // and runs the view dance on change. Every mode read in Core's decision
    // points is a state read — the window is a subscriber, not the source of
    // truth. The load-coupled surface cleanup (hiding the overlays for a
    // newly loaded folder) lives at the load site, not in this transition.
    QProperty<ViewMode> viewMode{MODE_DOCUMENT};
    // the GUI is waiting for the first ready frame: main defers showGui while
    // set, and onModelItemReady raises the window with the image already in it
    QProperty<bool> delayModel{false};
    // armed via DirectoryManager::whenListed: which image to show on a
    // folder-boundary transition is only knowable when the listing lands
    QProperty<qint64> pendingListedToken{0};
    QProperty<QString> currentFilePath{QString()};
    QProperty<QString> directoryPath{QString()};
    // the projection the views bind to — written by updateInfoString after
    // every mutation, read by the views on invalidation
    QProperty<CurrentInfo> currentInfo;
    QProperty<bool> slideshow{false};
    QProperty<bool> shuffle{false};
};
