/*
 * Core's editing, as an object: the edit contract (edit_template), the
 * in-memory transforms, the crop panel toggle, the save contract that commits
 * the edited image, discard and edit recovery. Owns no state of its own: it
 * reads State and the selection, edits through the model, asks navigation
 * whether the interaction state allows an edit (and lets it explain a
 * refusal), and speaks through the overlay, dialog and crop seams. The
 * coordinator's info projection is NOT owned: it is called back through
 * INavigationHost. The listing materializes on first load, so the model
 * arrives via attachModel.
 */

#include "editcontroller.h"
#include <utility>

#include "appstate.h"
#include "selectionquery.h"
#include "navigationcontroller.h"
#include "components/directorymodel.h"
#include "gui/dialoghost.h"
#include "gui/icropsurface.h"
#include "gui/ioverlayhost.h"
#include "settings.h"
#include "utils/fileoperations.h"
#include "utils/imagelib.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QUuid>

EditController::EditController(const State &state, const SelectionQuery &selection, NavigationController &nav,
                               INavigationHost *host, DialogHost *dialogs, IOverlayHost *overlays, ICropSurface *crop,
                               bool windowed, QObject *parent)
    : QObject(parent), m_state(state), m_selection(selection), m_nav(nav), m_host(host), m_dialogs(dialogs),
      m_overlays(overlays), m_crop(crop), m_windowed(windowed) {
}

void EditController::attachModel(DirectoryModel *model) {
    m_model = model;
}

void EditController::setOverlayHost(IOverlayHost *overlays) {
    m_overlays = overlays;
}

void EditController::rotateLeft() {
    rotateByDegrees(-90);
}

void EditController::rotateRight() {
    rotateByDegrees(90);
}

void EditController::toggleCropPanel() {
    if(!m_crop)
        return; // headless without a crop fake: nothing to toggle
    if(m_model->listing().isEmpty())
        return;
    // the folder view cannot reach this (no binding, and the panel refuses it
    // silently anyway); a running clock can, and it would move the image out
    // from under a selection in progress
    if(m_nav.interactionState() == NavigationController::InteractionState::Slideshow) {
        m_nav.refuseEdit();
        return;
    }
    if(m_crop->isActive() || m_state.hasActiveImage)
        m_crop->toggle();
}

void EditController::requestSavePath() {
    if(!m_windowed || m_model->listing().isEmpty())
        return;
    const QString path = m_selection.selectedPath();
    // an edited image earns a fresh target: Save As's quick path writes a
    // copy, and the original survives until Ctrl+S deliberately says
    // otherwise — the same law that keeps the disk off the bare keys
    const auto img = m_model->getImage(path);
    const QString suggested = (img && img->isEdited()) ? FileOperations::nextFreeVariantPath(path) : path;
    const QString destination = m_dialogs->getSaveFileName(suggested);
    if(destination.isEmpty())
        return;
    if(m_model->getImage(path) != img) {
        m_overlays->showError(tr("The selected image changed while the save dialog was open. Try again."));
        return;
    }
    if(saveFile(path, destination)) {
        m_overlays->showMessageSuccess(tr("File saved"));
        m_host->updateInfoString();
    }
}

void EditController::showResizeDialog() {
    if(m_model->listing().isEmpty())
        return;
    // the gate is here as well as in resize(): the dialog is modal but exec()
    // blocks only input, never the slideshow clock — a tick would fire behind
    // it and the edit would land on an image the user is no longer looking at
    if(m_nav.interactionState() == NavigationController::InteractionState::Slideshow) {
        m_nav.refuseEdit();
        return;
    }
    auto img = m_model->getImage(m_selection.selectedPath());
    if(img)
        m_dialogs->showResizeDialog(img->size());
}

// The edit contract: a transform applied to the selection, in memory. There is
// no "save" variant and no action name any more — the folder view's
// flush-immediately mode retired
// with the view-scoped keymap, because a thumbnail cannot show the result of a
// transform and writing behind an invisible confirmation is a silent rewrite.
// The disk is reached only by Ctrl+S / Ctrl+Shift+S, the same chords in both
// views, and `saveFile` is their contract.
template <typename... Args>
void EditController::edit_template(
    const std::function<std::unique_ptr<const QImage>(std::shared_ptr<const QImage>, Args...)> &editFunc,
    Args &&...as) {
    if(m_model->listing().isEmpty())
        return;
    // the edit contract is a property of the interaction state, not of this
    // call: a slideshow has no single image to edit — the clock moves on by
    // itself and the next tick's unloadExcept would discard whatever we wrote.
    // Checking the state (not "is the timer running") is what makes the rule
    // survive a future change to how the clock is stored.
    switch(m_nav.interactionState()) {
    case NavigationController::InteractionState::Slideshow:
        // refused BEFORE the save confirmation: asking "save the change?"
        // about an edit we are about to refuse would be a lie
        m_nav.refuseEdit();
        return;
    case NavigationController::InteractionState::FolderView:
    case NavigationController::InteractionState::Document: break;
    }
    const auto selection = m_selection.currentSelection();
    for(const auto &path : selection)
        m_model->editImage(path, [&](auto pixels) {
            return editFunc(std::move(pixels), as...);
        });
    m_host->updateInfoString();
}

void EditController::flipH() {
    edit_template({ImageLib::flippedH});
}

void EditController::flipV() {
    edit_template({ImageLib::flippedV});
}

void EditController::rotateByDegrees(int degrees) {
    edit_template({ImageLib::rotated}, degrees);
}

void EditController::resize(QSize size) {
    // the command stands on its own: DialogHost::resizeRequested connects
    // straight here. The folder view is already out of reach — its keymap has
    // no resize binding — so the only state still to refuse is the clock.
    if(m_nav.interactionState() == NavigationController::InteractionState::Slideshow) {
        m_nav.refuseEdit();
        return;
    }
    edit_template({ImageLib::scaled}, size, QI_FILTER_BILINEAR);
}

// No folder-view guard: the crop key and its surface live in the document
// view's keymap, so the folder view cannot reach this at all.
void EditController::crop(QRect rect) {
    edit_template({ImageLib::cropped}, rect);
}

void EditController::cropAndSave(QRect rect) {
    edit_template({ImageLib::cropped}, rect);
    saveFile(m_selection.selectedPath());
    m_host->updateInfoString();
}

bool EditController::saveFile(const QString &filePath) {
    return saveFile(filePath, filePath);
}

bool EditController::saveFile(const QString &filePath, const QString &newPath) {
    const auto result = m_model->saveFile(filePath, newPath);
    if(result == DirectoryModel::SaveResult::SourceChanged) {
        m_overlays->showError(tr("The original changed. Keep your edit by saving to a new PNG file."));
        return false;
    }
    if(result != DirectoryModel::SaveResult::Saved) {
        m_overlays->showError(tr("Could not save file"));
        return false;
    }
    m_overlays->hideSaveOverlay();
    // switch to the new file
    if(m_model->listing().containsFile(newPath) && m_state.currentFilePath != newPath) {
        discardEdits();
        if(m_state.viewMode.value() == MODE_DOCUMENT)
            m_nav.loadPath(newPath);
    }
    return true;
}

void EditController::saveCurrentFile() {
    saveCurrentFileAs(m_selection.selectedPath());
}

void EditController::saveCurrentFileAs(const QString &destPath) {
    if(m_model->listing().isEmpty())
        return;
    if(saveFile(m_selection.selectedPath(), destPath)) {
        m_overlays->showMessageSuccess(tr("File saved"));
        m_host->updateInfoString();
    }
}

void EditController::discardEdits() {
    if(m_model->listing().isEmpty())
        return;

    m_model->discardEdit(m_selection.selectedPath());
    m_overlays->hideSaveOverlay();
}

void EditController::recoverEdits() {
    if(!m_windowed)
        return;
    const auto edits = m_model->recoverableEdits();
    if(edits.isEmpty()) {
        m_overlays->showMessage(tr("No edits to recover."));
        return;
    }
    QStringList labels;
    for(const auto &edit : edits)
        labels.append(tr("%1 — %2").arg(edit.sourcePath, edit.id.toString(QUuid::WithoutBraces).left(8)));
    const int index = m_dialogs->chooseEditDraft(labels);
    if(index < 0)
        return;
    const auto &draft = edits.at(index);
    const QFileInfo original(draft.sourcePath);
    const QString directory = original.absoluteDir().exists() ? original.absolutePath() : QDir::homePath();
    const QString suggested = QDir(directory).filePath(original.completeBaseName() + tr("-recovered.png"));
    const QString destination = m_dialogs->getRecoveryFileName(suggested);
    if(destination.isEmpty())
        return;
    if(m_model->exportRecoveredEdit(draft.id, destination))
        m_overlays->showMessageSuccess(tr("Edit recovered to a new file."));
    else
        m_overlays->showError(tr("Could not export the draft. Choose a new PNG file."));
}
