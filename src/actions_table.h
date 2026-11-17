#pragma once

// The declarative table of actions. One row per action the keymap can bind:
// its stable name, what the UI calls it, what kind of command it is, where a
// first binding belongs, whether the editor may rebind it, and whether it
// changes the subject under the user's attention.
//
// Why a table and not six lists: before this file an action's identity was
// written down in six places that could disagree — the registry
// (initActions), the four default keymaps (initDefaults), the
// subject-changing set, the editor's gridHome, the R12 family of static
// checks, and the shortcut creator's dropdown. Renaming an action touched
// some of them; the checks caught what they scanned and missed what they did
// not. settings_table.h solved the same shape for settings, and this file is
// the same answer: the DESCRIPTION is data, the BEHAVIOR (resolution order,
// projection, overrides) stays code.
//
// Two deliberate choices:
//  - constexpr, so a row is visible to the compiler and to the Python rules
//    that read this file as text. That means token text is literal, which is
//    true on this tree's only built platform: InputMap's modifier names are
//    "Ctrl"/"Alt"/"Shift" (inputmap.cpp), and the platform-name assertion in
//    shortcut_scope_tests pins exactly that. If a platform ever answers
//    different names, that test fails BY NAME instead of the table silently
//    describing keys nobody can press. (The __APPLE__ branch initDefaults
//    used to carry is not represented: it was unobservable here, and an
//    unobservable branch is not a design constraint.)
//  - the defaults are a second, flat table keyed by (action, scope, token)
//    rather than a field on the row, because a binding's scope and an
//    action's home scope are different facts: contextMenu's home is Global
//    while its document-scope RMB is equally real, and one action may carry
//    pointer tokens (LMB_DoubleClick) that no key press produces.

#include <QString>

// scope/policy/flag live with the keymap's other public vocabulary
#include "components/actionmanager/actionmanager.h"

struct ActionDesc {
    const char *name = nullptr;     // the registry name: stable, never translated
    const char *display = nullptr;  // what the UI shows (translated at use)
    const char *category = nullptr; // grouping for the shortcut chooser
    Scope home = Scope::Document;   // which editor section offers an unbound action
    enum class Policy : unsigned char {
        User,    // the editor may rebind it freely
        System,  // destructive / the way out: visible, rebind needs a confirm
        Reserved // never offered (unused today; spelled so the gap is visible)
    } policy = Policy::User;
    bool subjectChanging = false; // a walk here swaps the file under a surface
};

struct DefaultBinding {
    const char *action = nullptr;
    Scope scope = Scope::Global;
    const char *token = nullptr;
};

namespace ActionTable {

constexpr ActionDesc MAKE(const char *name, const char *display, const char *category, Scope home,
                          ActionDesc::Policy policy, bool subjectChanging) {
    ActionDesc d;
    d.name = name;
    d.display = display;
    d.category = category;
    d.home = home;
    d.policy = policy;
    d.subjectChanging = subjectChanging;
    return d;
}

// ---- the actions ------------------------------------------------------------

constexpr ActionDesc kActions[] = {
    MAKE("nextImage", "Next image", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("prevImage", "Previous image", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("toggleFullscreen", "Toggle fullscreen", "View", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("fitWindow", "Fit window", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("fitWidth", "Fit width", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("fitNormal", "Original size", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("nextFitMode", "Next fit mode", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("toggleFitMode", "Toggle fit mode", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("resize", "Resize", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("rotateRight", "Rotate right", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("rotateLeft", "Rotate left", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("scrollUp", "Scroll up", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("scrollDown", "Scroll down", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("scrollLeft", "Scroll left", "View", Scope::Inspect, ActionDesc::Policy::User, false),
    MAKE("scrollRight", "Scroll right", "View", Scope::Inspect, ActionDesc::Policy::User, false),
    MAKE("zoomIn", "Zoom in", "View", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("zoomOut", "Zoom out", "View", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("zoomInCursor", "Zoom in at cursor", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("zoomOutCursor", "Zoom out at cursor", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("open", "Open file", "File", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("save", "Save", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("saveAs", "Save as", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("setWallpaper", "Set as wallpaper", "File", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("crop", "Crop", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("removeFile", "Delete permanently", "File", Scope::Global, ActionDesc::Policy::System, true),
    MAKE("copyFile", "Copy to folder", "File", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("moveFile", "Move to folder", "File", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("jumpToFirst", "First image", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("jumpToLast", "Last image", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("openSettings", "Preferences", "Window", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("closeFullScreenOrExit", "Close fullscreen / quit", "Window", Scope::Global, ActionDesc::Policy::System, true),
    MAKE("exit", "Quit", "Window", Scope::Global, ActionDesc::Policy::System, true),
    MAKE("flipH", "Flip horizontally", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("flipV", "Flip vertically", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("folderView", "Folder view", "Grid", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("pauseVideo", "Pause / resume", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("frameStep", "Next frame", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("frameStepBack", "Previous frame", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("documentView", "Document view", "Grid", Scope::Grid, ActionDesc::Policy::User, true),
    MAKE("toggleFolderView", "Toggle folder view", "Grid", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("moveToTrash", "Move to trash", "File", Scope::Global, ActionDesc::Policy::System, true),
    MAKE("reloadImage", "Reload image", "File", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("copyFileClipboard", "Copy file to clipboard", "File", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("copyPathClipboard", "Copy path to clipboard", "File", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("renameFile", "Rename", "File", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("contextMenu", "Context menu", "Window", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("toggleTransparencyGrid", "Transparency grid", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("sortByName", "Sort by name", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("sortByTime", "Sort by time", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("sortBySize", "Sort by size", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("toggleImageInfo", "Image information", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("toggleShuffle", "Shuffle", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("toggleScalingFilter", "Scaling filter", "Grid", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("toggleMute", "Mute", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("volumeUp", "Volume up", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("volumeDown", "Volume down", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("toggleSlideshow", "Slideshow", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("showInDirectory", "Show in file manager", "File", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("quickCopy", "Quick copy", "File", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("goUp", "Parent folder", "Navigation", Scope::Grid, ActionDesc::Policy::User, true),
    MAKE("discardEdits", "Discard edits", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("nextDirectory", "Next folder", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("prevDirectory", "Previous folder", "Navigation", Scope::Document, ActionDesc::Policy::User, true),
    MAKE("seekVideoForward", "Seek forward", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("seekVideoBackward", "Seek back", "Playback", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("lockZoom", "Lock zoom", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("lockView", "Lock view", "View", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("print", "Print", "Edit", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("toggleFullscreenInfoBar", "Fullscreen info bar", "Window", Scope::Global, ActionDesc::Policy::User, false),
    MAKE("pasteFile", "Paste image", "File", Scope::Document, ActionDesc::Policy::User, false),
    MAKE("pasteFilesHere", "Paste files here", "File", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectNext", "Select next", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectPrev", "Select previous", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectAbove", "Select above", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectBelow", "Select below", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridPageUp", "Page up", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridPageDown", "Page down", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectFirst", "Select first", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectLast", "Select last", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridSelectAll", "Select all", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
    MAKE("gridOpen", "Open selected", "Grid", Scope::Grid, ActionDesc::Policy::User, false),
};

// ---- the default bindings ---------------------------------------------------

constexpr DefaultBinding kDefaultBindings[] = {
    {"toggleFullscreen", Scope::Global, "F"},
    {"toggleFullscreen", Scope::Global, "F11"},
    {"toggleFullscreen", Scope::Global, "LMB_DoubleClick"},
    {"open", Scope::Global, "Ctrl+O"},
    {"exit", Scope::Global, "Ctrl+Q"},
    {"closeFullScreenOrExit", Scope::Global, "Esc"},
    {"moveToTrash", Scope::Global, "Del"},
    {"removeFile", Scope::Global, "Shift+Del"},
    {"copyFileClipboard", Scope::Global, "Ctrl+C"},
    {"copyPathClipboard", Scope::Global, "Ctrl+Shift+C"},
    {"renameFile", Scope::Global, "F2"},
    {"contextMenu", Scope::Global, "Menu"},
    {"showInDirectory", Scope::Global, "Ctrl+D"},
    {"toggleFullscreenInfoBar", Scope::Global, "Shift+F"},
    {"openSettings", Scope::Global, "P"},
    {"nextImage", Scope::Document, "Right"},
    {"prevImage", Scope::Document, "Left"},
    {"nextImage", Scope::Document, "XButton2"},
    {"prevImage", Scope::Document, "XButton1"},
    {"nextImage", Scope::Document, "WheelDown"},
    {"prevImage", Scope::Document, "WheelUp"},
    {"prevImage", Scope::Document, "PgUp"},
    {"nextImage", Scope::Document, "PgDown"},
    {"toggleFitMode", Scope::Document, "Space"},
    {"nextFitMode", Scope::Document, "Shift+Space"},
    {"fitWindow", Scope::Document, "1"},
    {"fitWidth", Scope::Document, "2"},
    {"fitNormal", Scope::Document, "3"},
    {"resize", Scope::Document, "R"},
    {"flipH", Scope::Document, "H"},
    {"flipV", Scope::Document, "V"},
    {"copyFile", Scope::Document, "C"},
    {"moveFile", Scope::Document, "M"},
    {"contextMenu", Scope::Document, "RMB"},
    {"pasteFile", Scope::Document, "Ctrl+V"},
    {"folderView", Scope::Document, "Enter"},
    {"folderView", Scope::Document, "Backspace"},
    {"rotateRight", Scope::Document, "Ctrl+R"},
    {"rotateLeft", Scope::Document, "Ctrl+L"},
    {"zoomInCursor", Scope::Document, "Ctrl+WheelUp"},
    {"zoomOutCursor", Scope::Document, "Ctrl+WheelDown"},
    {"zoomIn", Scope::Document, "="},
    {"zoomIn", Scope::Document, "Ctrl+="},
    {"zoomIn", Scope::Document, "+"},
    {"zoomIn", Scope::Document, "Ctrl++"},
    {"zoomOut", Scope::Document, "-"},
    {"zoomOut", Scope::Document, "Ctrl+-"},
    {"zoomOut", Scope::Document, "Ctrl+Down"},
    {"zoomIn", Scope::Document, "Ctrl+Up"},
    {"scrollUp", Scope::Document, "Up"},
    {"scrollDown", Scope::Document, "Down"},
    {"save", Scope::Document, "Ctrl+S"},
    {"saveAs", Scope::Document, "Ctrl+Shift+S"},
    {"setWallpaper", Scope::Document, "W"},
    {"crop", Scope::Document, "X"},
    {"print", Scope::Document, "Ctrl+P"},
    {"jumpToFirst", Scope::Document, "Home"},
    {"jumpToLast", Scope::Document, "End"},
    {"seekVideoForward", Scope::Document, "Ctrl+Right"},
    {"seekVideoBackward", Scope::Document, "Ctrl+Left"},
    {"frameStepBack", Scope::Document, ","},
    {"frameStep", Scope::Document, "."},
    {"discardEdits", Scope::Document, "Ctrl+Z"},
    {"nextDirectory", Scope::Document, "Shift+Right"},
    {"prevDirectory", Scope::Document, "Shift+Left"},
    {"toggleSlideshow", Scope::Document, "`"},
    {"toggleShuffle", Scope::Document, "Ctrl+`"},
    {"toggleImageInfo", Scope::Document, "I"},
    {"reloadImage", Scope::Document, "F5"},
    {"gridSelectPrev", Scope::Grid, "Left"},
    {"gridSelectNext", Scope::Grid, "Right"},
    {"gridSelectAbove", Scope::Grid, "Up"},
    {"gridSelectBelow", Scope::Grid, "Down"},
    {"gridPageUp", Scope::Grid, "PgUp"},
    {"gridPageDown", Scope::Grid, "PgDown"},
    {"gridSelectFirst", Scope::Grid, "Home"},
    {"gridSelectLast", Scope::Grid, "End"},
    {"gridOpen", Scope::Grid, "Enter"},
    {"goUp", Scope::Grid, "Backspace"},
    {"gridSelectAll", Scope::Grid, "Ctrl+A"},
    {"pasteFilesHere", Scope::Grid, "Ctrl+V"},
    {"scrollLeft", Scope::Inspect, "Left"},
    {"scrollRight", Scope::Inspect, "Right"},
};

// ---- derived views ----------------------------------------------------------
//
// The rows are the data; these are the three questions the UI asks of them.
// They live beside the table rather than in ActionManager so that any reader
// (the settings editor, the shortcut chooser, a future command palette) asks
// the same source — and so actionmanager.h does not have to know ActionDesc.

inline const ActionDesc *descFor(const QString &name) {
    for(const ActionDesc &row : kActions)
        if(name == QLatin1String(row.name))
            return &row;
    return nullptr;
}

} // namespace ActionTable
