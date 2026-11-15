#pragma once

#include <QtGlobal>

// The declarative table of scalar leaf settings. One row per key: the key, its
// type, its default, and (for numbers) the valid range. The typed accessors in
// Settings (load/store) read this table, so a hand-edited config value can
// never flow past a clamp again, and the audit test walks every row.
//
// NOT in the table, deliberately: composite or computed values (scripts,
// videoFormats, supportedFormats, cacheDir fallback, scalingFilter's
// build-conditional default, zoomLevels) — hard-casing them into a scalar
// table would recreate the boilerplate the table exists to remove.

struct SettingDesc {
    enum class Kind : unsigned char {
        Bool,
        Int,
        Real,
        Str
    };
    const char *key = nullptr;
    Kind kind = Kind::Bool;
    // four parallel defaults instead of a union: switching a union's active
    // member is not a constant expression before C++20
    bool defB = false;
    qlonglong defI = 0;
    double defR = 0.0;
    const char *defS = nullptr;
    double lo = 0.0, hi = 0.0; // numeric clamp bounds
    bool hasLo = false, hasHi = false;
    // Two policies exist for a value outside [lo, hi] and they answer
    // differently: clamp moves it to the nearest legal value, reset discards it
    // for the default. Enum-valued settings want reset (an out-of-range mode is
    // a broken value, not a request for the last mode), a pixel size wants
    // clamp (60 is a real size that only overshot the panel minimum).
    bool resetOnRange = false;

    constexpr bool clamped() const {
        return hasLo || hasHi;
    }
};

namespace SettingRow {
static constexpr SettingDesc Bool(const char *key, bool def) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Bool;
    d.defB = def;
    return d;
}
static constexpr SettingDesc Int(const char *key, qlonglong def) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Int;
    d.defI = def;
    return d;
}
static constexpr SettingDesc Int(const char *key, qlonglong def, qlonglong lo) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Int;
    d.defI = def;
    d.lo = lo;
    d.hasLo = true;
    return d;
}
static constexpr SettingDesc Int(const char *key, qlonglong def, qlonglong lo, qlonglong hi) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Int;
    d.defI = def;
    d.lo = lo;
    d.hi = hi;
    d.hasLo = true;
    d.hasHi = true;
    return d;
}
// the same bounds, the other policy: out of range means "take the default"
static constexpr SettingDesc IntReset(const char *key, qlonglong def, qlonglong lo) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Int;
    d.defI = def;
    d.lo = lo;
    d.hasLo = true;
    d.resetOnRange = true;
    return d;
}
static constexpr SettingDesc IntReset(const char *key, qlonglong def, qlonglong lo, qlonglong hi) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Int;
    d.defI = def;
    d.lo = lo;
    d.hi = hi;
    d.hasLo = true;
    d.hasHi = true;
    d.resetOnRange = true;
    return d;
}
static constexpr SettingDesc REAL(const char *key, double def, double lo, double hi) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Real;
    d.defR = def;
    d.lo = lo;
    d.hi = hi;
    d.hasLo = true;
    d.hasHi = true;
    return d;
}
static constexpr SettingDesc STR(const char *key, const char *def) {
    SettingDesc d;
    d.key = key;
    d.kind = SettingDesc::Kind::Str;
    d.defS = def;
    return d;
}
} // namespace SettingRow

using SettingRow::Bool;
using SettingRow::Int;
using SettingRow::IntReset;
using SettingRow::REAL;
using SettingRow::STR;

// ---- rows -------------------------------------------------------------------

inline constexpr SettingDesc K_VideoPlayback = Bool("videoPlayback", true);
inline constexpr SettingDesc K_UseSystemColorScheme = Bool("useSystemColorScheme", false);
inline constexpr SettingDesc K_ShowChangelogs = Bool("showChangelogs", true);
inline constexpr SettingDesc K_BlurBackground = Bool("blurBackground", true);
inline constexpr SettingDesc K_PlayVideoSounds = Bool("playVideoSounds", false);
inline constexpr SettingDesc K_UsePreloader = Bool("usePreloader", true);
inline constexpr SettingDesc K_FullscreenMode = Bool("openInFullscreen", false);
inline constexpr SettingDesc K_PanelEnabled = Bool("panelEnabled", true);
inline constexpr SettingDesc K_PanelFullscreenOnly = Bool("panelFullscreenOnly", true);
inline constexpr SettingDesc K_PanelPinned = Bool("panelPinned", false);
inline constexpr SettingDesc K_LoopSlideshow = Bool("loopSlideshow", false);
inline constexpr SettingDesc K_SquareThumbnails = Bool("squareThumbnails", false);
inline constexpr SettingDesc K_TransparencyGrid = Bool("drawTransparencyGrid", false);
inline constexpr SettingDesc K_EnableSmoothScroll = Bool("enableSmoothScroll", true);
inline constexpr SettingDesc K_UseThumbnailCache = Bool("thumbnailCache", true);
inline constexpr SettingDesc K_SmoothUpscaling = Bool("smoothUpscaling", true);
inline constexpr SettingDesc K_ExpandImage = Bool("expandImage", false);
inline constexpr SettingDesc K_SmoothAnimatedImages = Bool("smoothAnimatedImages", true);
inline constexpr SettingDesc K_InfoBarFullscreen = Bool("infoBarFullscreen", true);
inline constexpr SettingDesc K_InfoBarWindowed = Bool("infoBarWindowed", false);
inline constexpr SettingDesc K_WindowTitleExtendedInfo = Bool("windowTitleExtendedInfo", true);
inline constexpr SettingDesc K_CursorAutohide = Bool("cursorAutohiding", true);
inline constexpr SettingDesc K_FirstRun = Bool("firstRun", true);
inline constexpr SettingDesc K_ShowSaveOverlay = Bool("showSaveOverlay", true);
inline constexpr SettingDesc K_ConfirmTrash = Bool("confirmTrash", true);
inline constexpr SettingDesc K_UnloadThumbs = Bool("unloadThumbs", true);
inline constexpr SettingDesc K_JxlAnimation = Bool("jxlAnimation", false);
inline constexpr SettingDesc K_AutoResizeWindow = Bool("autoResizeWindow", false);
inline constexpr SettingDesc K_UseFixedZoomLevels = Bool("useFixedZoomLevels", false);
inline constexpr SettingDesc K_UnlockMinZoom = Bool("unlockMinZoom", true);
inline constexpr SettingDesc K_TrackpadDetection = Bool("trackpadDetection", true);
inline constexpr SettingDesc K_ClickableEdges = Bool("clickableEdges", false);
inline constexpr SettingDesc K_ClickableEdgesVisible = Bool("clickableEdgesVisible", true);
inline constexpr SettingDesc K_ShowHiddenFiles = Bool("showHiddenFiles", false);
inline constexpr SettingDesc K_PanelCenterSelection = Bool("panelCenterSelection", false);
inline constexpr SettingDesc K_FolderViewIconSize = Int("folderViewIconSize", 120);
inline constexpr SettingDesc K_DefaultFitMode = IntReset("defaultFitMode", 0, 0, 3);
inline constexpr SettingDesc K_FocusPointIn1to1Mode = IntReset("focusPointIn1to1Mode", 1, 0, 2);
inline constexpr SettingDesc K_DefaultCropAction = IntReset("defaultCropAction", 0, 0, 1);
inline constexpr SettingDesc K_DefaultViewMode = IntReset("defaultViewMode", 0, 0, 1);
inline constexpr SettingDesc K_SortingMode = IntReset("sortingMode", 0, 0, 5);
inline constexpr SettingDesc K_FolderEndAction = IntReset("folderEndAction", 0, 0, 2);
inline constexpr SettingDesc K_FolderViewMode = IntReset("folderViewMode", 2, 0, 2);
inline constexpr SettingDesc K_ThumbPanelStyle = IntReset("thumbPanelStyle", 1, 0, 1);
inline constexpr SettingDesc K_ImageScrolling = IntReset("imageScrolling", 1, 0, 2);
inline constexpr SettingDesc K_ZoomIndicatorMode = IntReset("zoomIndicatorMode", 0, 0, 2);
inline constexpr SettingDesc K_MemoryAllocationLimit = Int("memoryAllocationLimit", 1024, 512, 8192);
inline constexpr SettingDesc K_SlideshowInterval = IntReset("slideshowInterval", 3000, 1);
inline constexpr SettingDesc K_ThumbnailerThreads = IntReset("thumbnailerThreads", 4, 1);
inline constexpr SettingDesc K_PanelPreviewsSize = Int("panelPreviewsSize", 140, 100, 250);
inline constexpr SettingDesc K_AutoResizeLimit = IntReset("autoResizeLimit", 90, 30, 100);
inline constexpr SettingDesc K_JPEGSaveQuality = Int("JPEGSaveQuality", 95, 0, 100);
inline constexpr SettingDesc K_ExpandLimit = Int("expandLimit", 2);
inline constexpr SettingDesc K_LastVerMajor = Int("lastVerMajor", 0);
inline constexpr SettingDesc K_LastVerMinor = Int("lastVerMinor", 0);
inline constexpr SettingDesc K_LastVerMicro = Int("lastVerMicro", 0);

inline constexpr SettingDesc K_ZoomStep = REAL("zoomStep", 0.2, 0.01, 0.5);
inline constexpr SettingDesc K_MouseScrollingSpeed = REAL("mouseScrollingSpeed", 1.0, 0.5, 2.0);
inline constexpr SettingDesc K_BackgroundOpacity = REAL("backgroundOpacity", 1.0, 0.0, 1.0);

inline constexpr SettingDesc K_PanelPosition = STR("panelPosition", "top");
inline constexpr SettingDesc K_MpvBinary = STR("mpvBinary", "");
inline constexpr SettingDesc K_QuickCopyDirectory = STR("quickCopyDirectory", "");
inline constexpr SettingDesc K_Language = STR("language", "en_US");

// The audit test walks this list; keep it in sync when adding a row.
inline constexpr const SettingDesc *kAllSettings[] = {
    &K_VideoPlayback,
    &K_UseSystemColorScheme,
    &K_ShowChangelogs,
    &K_BlurBackground,
    &K_PlayVideoSounds,
    &K_UsePreloader,
    &K_FullscreenMode,
    &K_PanelEnabled,
    &K_PanelFullscreenOnly,
    &K_PanelPinned,
    &K_LoopSlideshow,
    &K_SquareThumbnails,
    &K_TransparencyGrid,
    &K_EnableSmoothScroll,
    &K_UseThumbnailCache,
    &K_SmoothUpscaling,
    &K_ExpandImage,
    &K_SmoothAnimatedImages,
    &K_InfoBarFullscreen,
    &K_InfoBarWindowed,
    &K_WindowTitleExtendedInfo,
    &K_CursorAutohide,
    &K_FirstRun,
    &K_ShowSaveOverlay,
    &K_ConfirmTrash,
    &K_UnloadThumbs,
    &K_JxlAnimation,
    &K_AutoResizeWindow,
    &K_UseFixedZoomLevels,
    &K_UnlockMinZoom,
    &K_TrackpadDetection,
    &K_ClickableEdges,
    &K_ClickableEdgesVisible,
    &K_ShowHiddenFiles,
    &K_SortingMode,
    &K_FolderEndAction,
    &K_FolderViewMode,
    &K_ThumbPanelStyle,
    &K_ImageScrolling,
    &K_ZoomIndicatorMode,
    &K_MemoryAllocationLimit,
    &K_SlideshowInterval,
    &K_ThumbnailerThreads,
    &K_PanelPreviewsSize,
    &K_AutoResizeLimit,
    &K_JPEGSaveQuality,
    &K_ExpandLimit,
    &K_LastVerMajor,
    &K_LastVerMinor,
    &K_LastVerMicro,
    &K_ZoomStep,
    &K_MouseScrollingSpeed,
    &K_BackgroundOpacity,
    &K_PanelPosition,
    &K_MpvBinary,
    &K_Language,
};
