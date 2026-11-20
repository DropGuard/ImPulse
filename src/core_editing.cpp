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
#include "gui/icropsurface.h"

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
    if(!m_crop)
        return; // headless without a crop fake: nothing to toggle
    if(m_model->isEmpty())
        return;
    // the folder view cannot reach this (no binding, and the panel refuses it
    // silently anyway); a running clock can, and it would move the image out
    // from under a selection in progress
    if(m_nav->interactionState() == NavigationController::InteractionState::Slideshow) {
        m_nav->refuseEdit();
        return;
    }
    if(m_crop->isActive() || m_state.hasActiveImage)
        m_crop->toggle();
}

void Core::requestSavePath() {
    if(m_model->isEmpty())
        return;
    const QString path = selectedPath();
    // an edited image earns a fresh target: Save As's quick path writes a
    // copy, and the original survives until Ctrl+S deliberately says
    // otherwise — the same law that keeps the disk off the bare keys
    const auto img = m_model->getImage(path);
    const QString suggested = (img && img->isEdited()) ? FileOperations::nextFreeVariantPath(path) : path;
    m_dialogs->showSaveDialog(suggested);
}

void Core::showResizeDialog() {
    if(m_model->isEmpty())
        return;
    // the gate is here as well as in resize(): the dialog is modal but exec()
    // blocks only input, never the slideshow clock — a tick would fire behind
    // it and the edit would land on an image the user is no longer looking at
    if(m_nav->interactionState() == NavigationController::InteractionState::Slideshow) {
        m_nav->refuseEdit();
        return;
    }
    auto img = m_model->getImage(selectedPath());
    if(img)
        m_dialogs->showResizeDialog(img->size());
}

std::shared_ptr<ImageStatic> Core::getEditableImage(const QString &filePath) {
    return std::dynamic_pointer_cast<ImageStatic>(m_model->getImage(filePath));
}

// The edit contract: a transform applied to the selection, in memory. There is
// no "save" variant and no action name any more — the folder view's
// flush-immediately mode retired
// with the view-scoped keymap, because a thumbnail cannot show the result of a
// transform and writing behind an invisible confirmation is a silent rewrite.
// The disk is reached only by Ctrl+S / Ctrl+Shift+S, the same chords in both
// views, and `saveFile` is their contract.
template <typename... Args>
void Core::edit_template(
    const std::function<std::unique_ptr<const QImage>(std::shared_ptr<const QImage>, Args...)> &editFunc,
    Args &&...as) {
    if(m_model->isEmpty())
        return;
    // the edit contract is a property of the interaction state, not of this
    // call: a slideshow has no single image to edit — the clock moves on by
    // itself and the next tick's unloadExcept would discard whatever we wrote.
    // Checking the state (not "is the timer running") is what makes the rule
    // survive a future change to how the clock is stored.
    switch(m_nav->interactionState()) {
    case NavigationController::InteractionState::Slideshow:
        // refused BEFORE the save confirmation: asking "save the change?"
        // about an edit we are about to refuse would be a lie
        m_nav->refuseEdit();
        return;
    case NavigationController::InteractionState::FolderView:
    case NavigationController::InteractionState::Document: break;
    }
    const auto selection = currentSelection();
    for(const auto &path : selection) {
        auto img = getEditableImage(path);
        if(!img)
            continue;
        // no std::forward here: the pack is used once per selected file, and a
        // forwarded rvalue is empty from the second file on. Every current call
        // site passes lvalues, so forwarding bought nothing but the trap.
        img->setEditedImage(editFunc(img->getImage(), as...));
        m_model->updateImage(path, std::static_pointer_cast<Image>(img));
    }
    updateInfoString();
}

void Core::flipH() {
    edit_template({ImageLib::flippedH});
}

void Core::flipV() {
    edit_template({ImageLib::flippedV});
}

void Core::rotateByDegrees(int degrees) {
    edit_template({ImageLib::rotated}, degrees);
}

void Core::resize(QSize size) {
    // the command stands on its own: DialogHost::resizeRequested connects
    // straight here. The folder view is already out of reach — its keymap has
    // no resize binding — so the only state still to refuse is the clock.
    if(m_nav->interactionState() == NavigationController::InteractionState::Slideshow) {
        m_nav->refuseEdit();
        return;
    }
    edit_template({ImageLib::scaled}, size, QI_FILTER_BILINEAR);
}

// No folder-view guard: the crop key and its surface live in the document
// view's keymap, so the folder view cannot reach this at all.
void Core::crop(QRect rect) {
    edit_template({ImageLib::cropped}, rect);
}

void Core::cropAndSave(QRect rect) {
    edit_template({ImageLib::cropped}, rect);
    saveFile(selectedPath());
    updateInfoString();
}

bool Core::saveFile(const QString &filePath) {
    return saveFile(filePath, filePath);
}

bool Core::saveFile(const QString &filePath, const QString &newPath) {
    if(!m_model->saveFile(filePath, newPath))
        return false;
    m_overlays->hideSaveOverlay();
    // switch to the new file
    if(m_model->containsFile(newPath) && m_state.currentFilePath != newPath) {
        discardEdits();
        if(m_state.viewMode.value() == MODE_DOCUMENT)
            loadPath(newPath);
    }
    return true;
}

void Core::saveCurrentFile() {
    saveCurrentFileAs(selectedPath());
}

void Core::saveCurrentFileAs(const QString &destPath) {
    if(m_model->isEmpty())
        return;
    if(saveFile(selectedPath(), destPath)) {
        m_overlays->showMessageSuccess(tr("File saved"));
        updateInfoString();
    } else {
        m_overlays->showError(tr("Could not save file"));
    }
}

void Core::discardEdits() {
    if(m_model->isEmpty())
        return;

    std::shared_ptr<Image> img = m_model->getImage(selectedPath());
    if(img && img->type() == STATIC) {
        auto imgStatic = dynamic_cast<ImageStatic *>(img.get());
        imgStatic->discardEditedImage();
        m_model->updateImage(selectedPath(), img);
    }
    m_overlays->hideSaveOverlay();
}
