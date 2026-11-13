/*
 * Core's editing half: the edit contract (edit_template), the in-memory
 * transforms, the crop panel toggle, and the save contract that commits the
 * edited image. Same class, partial implementation — core.cpp keeps
 * composition, wiring and the display sink.
 */

#include "core.h"
#include <utility>

#include "utils/folderend.h"

#include "gui/windowshell.h"
#include "gui/overlayhost.h"
#include "gui/dialoghost.h"
#include "gui/cropcontroller.h"

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

void Core::rotateLeft() {
    rotateByDegrees(-90);
}

void Core::rotateRight() {
    rotateByDegrees(90);
}

void Core::toggleCropPanel() {
    if(!mw)
        return; // crop is a surface over the window; headless has none
    if(model->isEmpty())
        return;
    if(mw->crop()->isCropPanelActive() || state.hasActiveImage)
        mw->crop()->trigger();
}

void Core::requestSavePath() {
    if(model->isEmpty())
        return;
    const QString path = selectedPath();
    // an edited image earns a fresh target: Save As's quick path writes a
    // copy, and the original survives until Ctrl+S deliberately says
    // otherwise — the same law that keeps the disk off the bare keys
    const auto img = model->getImage(path);
    const QString suggested = (img && img->isEdited()) ? FileOperations::nextFreeVariantPath(path) : path;
    dialogs_->showSaveDialog(suggested);
}

void Core::showResizeDialog() {
    if(model->isEmpty())
        return;
    auto img = model->getImage(selectedPath());
    if(img)
        dialogs_->showResizeDialog(img->size());
}

std::shared_ptr<ImageStatic> Core::getEditableImage(const QString &filePath) {
    return std::dynamic_pointer_cast<ImageStatic>(model->getImage(filePath));
}

template <typename... Args>
void Core::edit_template(
    bool save, const QString &action,
    const std::function<std::unique_ptr<const QImage>(std::shared_ptr<const QImage>, Args...)> &editFunc,
    Args &&...as) {
    if(model->isEmpty())
        return;
    if(save && !ui_->showConfirmation(action, tr("Perform action \"") + action + "\"? \n\n" +
                                                  tr("Changes will be saved immediately.")))
        return;
    const auto selection = currentSelection();
    for(const auto &path : selection) {
        auto img = getEditableImage(path);
        if(!img)
            continue;
        // no std::forward here: the pack is used once per selected file, and a
        // forwarded rvalue is empty from the second file on. Every current call
        // site passes lvalues, so forwarding bought nothing but the trap.
        img->setEditedImage(editFunc(img->getImage(), as...));
        model->updateImage(path, std::static_pointer_cast<Image>(img));
        if(save) {
            saveFile(path);
            if(state.currentFilePath != path)
                model->unload(path);
        }
    }
    updateInfoString();
}

void Core::flipH() {
    edit_template((state.viewMode.value() == MODE_FOLDERVIEW), tr("Flip horizontal"), {ImageLib::flippedH});
}

void Core::flipV() {
    edit_template((state.viewMode.value() == MODE_FOLDERVIEW), tr("Flip vertical"), {ImageLib::flippedV});
}

void Core::rotateByDegrees(int degrees) {
    edit_template((state.viewMode.value() == MODE_FOLDERVIEW), tr("Rotate"), {ImageLib::rotated}, degrees);
}

void Core::resize(QSize size) {
    edit_template(false, tr("Resize"), {ImageLib::scaled}, size, QI_FILTER_BILINEAR);
}

void Core::crop(QRect rect) {
    if(state.viewMode.value() == MODE_FOLDERVIEW)
        return;
    edit_template(false, tr("Crop"), {ImageLib::cropped}, rect);
}

void Core::cropAndSave(QRect rect) {
    if(state.viewMode.value() == MODE_FOLDERVIEW)
        return;
    edit_template(false, tr("Crop"), {ImageLib::cropped}, rect);
    saveFile(selectedPath());
    updateInfoString();
}

bool Core::saveFile(const QString &filePath) {
    return saveFile(filePath, filePath);
}

bool Core::saveFile(const QString &filePath, const QString &newPath) {
    if(!model->saveFile(filePath, newPath))
        return false;
    overlays_->hideSaveOverlay();
    // switch to the new file
    if(model->containsFile(newPath) && state.currentFilePath != newPath) {
        discardEdits();
        if(state.viewMode.value() == MODE_DOCUMENT)
            loadPath(newPath);
    }
    return true;
}

void Core::saveCurrentFile() {
    saveCurrentFileAs(selectedPath());
}

void Core::saveCurrentFileAs(const QString &destPath) {
    if(model->isEmpty())
        return;
    if(saveFile(selectedPath(), destPath)) {
        overlays_->showMessageSuccess(tr("File saved"));
        updateInfoString();
    } else {
        overlays_->showError(tr("Could not save file"));
    }
}

void Core::discardEdits() {
    if(model->isEmpty())
        return;

    std::shared_ptr<Image> img = model->getImage(selectedPath());
    if(img && img->type() == STATIC) {
        auto imgStatic = dynamic_cast<ImageStatic *>(img.get());
        imgStatic->discardEditedImage();
        model->updateImage(selectedPath(), img);
    }
    overlays_->hideSaveOverlay();
}
