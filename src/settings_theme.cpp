/*
 * The theme manager half of Settings — the move the old comment above
 * loadStylesheet spent years promising ("temporarily, will be moved to some
 * sort of theme manager class"): stylesheet generation, theme/scheme loading
 * and saving, the color-scheme accessors. Same class, partial
 * implementation; Settings::load/store are member templates instantiated in
 * settings.cpp, so this TU consumes them through the class interface and the
 * plain setting rows (useSystemColorScheme and friends) stay in the table.
 */

#include <type_traits>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include "settings.h"

#include <QStandardPaths>

//------------------------------------------------------------------------------
void Settings::loadStylesheet() {
    // the stylesheet styles widgets — a QCoreApplication run (headless test
    // suites) has none to style, and the font metrics below need QGuiApplication
    if(qobject_cast<QApplication *>(qApp) == nullptr)
        return;
    // stylesheet template file
    QFile file(":/res/styles/style-template.qss");
    if(file.open(QFile::ReadOnly)) {
        QString styleSheet = QLatin1String(file.readAll());

        // --- color scheme ---------------------------------------------
        auto colors = colorScheme();
        // tint color for system windows
        QPalette p;
        QColor sys_text = p.text().color();
        QColor sys_window = p.window().color();
        QColor sys_window_tinted, sys_window_tinted_lc, sys_window_tinted_lc2, sys_window_tinted_hc,
            sys_window_tinted_hc2;
        if(sys_window.valueF() <= 0.45f) {
            // dark system theme
            sys_window_tinted_lc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 6);
            sys_window_tinted_lc.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 14);
            sys_window_tinted.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 20);
            sys_window_tinted_hc.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 35);
            sys_window_tinted_hc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() + 50);
        } else {
            // light system theme
            sys_window_tinted_lc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 6);
            sys_window_tinted_lc.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 14);
            sys_window_tinted.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 20);
            sys_window_tinted_hc.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 35);
            sys_window_tinted_hc2.setHsv(sys_window.hue(), sys_window.saturation(), sys_window.value() - 50);
        }

        // --- widget sizes ---------------------------------------------
        auto fnt = QGuiApplication::font();
        QFontMetrics fm(fnt);
        // todo: use precise values for ~9-11 point sizes
        int font_small = qMax(static_cast<int>(fnt.pointSize() * 0.9f), 8);
        int font_large = static_cast<int>(fnt.pointSize() * 1.8f);
        int text_height = fm.height();
        int text_padding = static_cast<int>(text_height * 0.10f);
        int text_padding_large = static_cast<int>(text_height * 0.25f);

        // folderview top panel item sizes
        int top_panel_v_margin = 4;
        // ensure at least 4px so its not too thin
        int top_panel_text_padding = qMax(text_padding, 4);
        // scale with font, 38px base size
        int top_panel_height = qMax((text_height + top_panel_text_padding * 2 + top_panel_v_margin * 2), 38);

        // overlay headers
        // 32px base size
        int overlay_header_size = qMax(text_height + text_padding * 2, 30);

        // todo
        int button_height = text_height + text_padding_large * 2;

        // pseudo-dpi to scale some widget widths
        int text_height_base = 22;
        qreal pDpr = qMax((static_cast<qreal>(text_height) / text_height_base), 1.0);
        int context_menu_width = 212 * pDpr;
        int context_menu_button_height = 32 * pDpr;
        int rename_overlay_width = 380 * pDpr;

        //qDebug()<< "dpr=" << qApp->devicePixelRatio() << "pDpr=" << pDpr;

        // --- write variables into stylesheet --------------------------
        styleSheet.replace("%font_small%", QString::number(font_small) + "pt");
        styleSheet.replace("%font_large%", QString::number(font_large) + "pt");
        styleSheet.replace("%button_height%", QString::number(button_height) + "px");
        styleSheet.replace("%top_panel_height%", QString::number(top_panel_height) + "px");
        styleSheet.replace("%overlay_header_size%", QString::number(overlay_header_size) + "px");
        styleSheet.replace("%context_menu_width%", QString::number(context_menu_width) + "px");
        styleSheet.replace("%context_menu_button_height%", QString::number(context_menu_button_height) + "px");
        styleSheet.replace("%rename_overlay_width%", QString::number(rename_overlay_width) + "px");

        styleSheet.replace("%icontheme%", "light");
        // Qt::Popup can't do transparency under windows, use square window
#ifdef _WIN32
        styleSheet.replace("%contextmenu_border_radius%", "0px");
#else
        styleSheet.replace("%contextmenu_border_radius%", "3px");
#endif
        styleSheet.replace("%sys_window%", sys_window.name());
        styleSheet.replace("%sys_window_tinted%", sys_window_tinted.name());
        styleSheet.replace("%sys_window_tinted_lc%", sys_window_tinted_lc.name());
        styleSheet.replace("%sys_window_tinted_lc2%", sys_window_tinted_lc2.name());
        styleSheet.replace("%sys_window_tinted_hc%", sys_window_tinted_hc.name());
        styleSheet.replace("%sys_window_tinted_hc2%", sys_window_tinted_hc2.name());
        styleSheet.replace("%sys_text_secondary_rgba%", "rgba(" + QString::number(sys_text.red()) + "," +
                                                            QString::number(sys_text.green()) + "," +
                                                            QString::number(sys_text.blue()) + ",50%)");

        styleSheet.replace("%button%", colors.button.name());
        styleSheet.replace("%button_hover%", colors.button_hover.name());
        styleSheet.replace("%button_pressed%", colors.button_pressed.name());
        styleSheet.replace("%panel_button%", colors.panel_button.name());
        styleSheet.replace("%panel_button_hover%", colors.panel_button_hover.name());
        styleSheet.replace("%panel_button_pressed%", colors.panel_button_pressed.name());
        styleSheet.replace("%widget%", colors.widget.name());
        styleSheet.replace("%widget_border%", colors.widget_border.name());
        styleSheet.replace("%folderview%", colors.folderview.name());
        styleSheet.replace("%folderview_topbar%", colors.folderview_topbar.name());
        styleSheet.replace("%folderview_hc%", colors.folderview_hc.name());
        styleSheet.replace("%folderview_hc2%", colors.folderview_hc2.name());
        styleSheet.replace("%accent%", colors.accent.name());
        styleSheet.replace("%input_field_focus%", colors.input_field_focus.name());
        styleSheet.replace("%overlay%", colors.overlay.name());
        styleSheet.replace("%icons%", colors.icons.name());
        styleSheet.replace("%text_hc2%", colors.text_hc2.name());
        styleSheet.replace("%text_hc%", colors.text_hc.name());
        styleSheet.replace("%text%", colors.text.name());
        styleSheet.replace("%overlay_text%", colors.overlay_text.name());
        styleSheet.replace("%text_lc%", colors.text_lc.name());
        styleSheet.replace("%text_lc2%", colors.text_lc2.name());
        styleSheet.replace("%scrollbar%", colors.scrollbar.name());
        styleSheet.replace("%scrollbar_hover%", colors.scrollbar_hover.name());
        styleSheet.replace("%folderview_button_hover%", colors.folderview_button_hover.name());
        styleSheet.replace("%folderview_button_pressed%", colors.folderview_button_pressed.name());
        styleSheet.replace("%text_secondary_rgba%", "rgba(" + QString::number(colors.text.red()) + "," +
                                                        QString::number(colors.text.green()) + "," +
                                                        QString::number(colors.text.blue()) + ",62%)");
        styleSheet.replace("%accent_hover_rgba%", "rgba(" + QString::number(colors.accent.red()) + "," +
                                                      QString::number(colors.accent.green()) + "," +
                                                      QString::number(colors.accent.blue()) + ",65%)");
        styleSheet.replace("%overlay_rgba%", "rgba(" + QString::number(colors.overlay.red()) + "," +
                                                 QString::number(colors.overlay.green()) + "," +
                                                 QString::number(colors.overlay.blue()) + ",90%)");
        styleSheet.replace("%fv_backdrop_rgba%", "rgba(" + QString::number(colors.folderview_hc2.red()) + "," +
                                                     QString::number(colors.folderview_hc2.green()) + "," +
                                                     QString::number(colors.folderview_hc2.blue()) + ",80%)");
        // do not show separator line if topbar color matches folderview
        if(colors.folderview != colors.folderview_topbar)
            styleSheet.replace("%topbar_border_rgba%", "rgba(0,0,0,14%)");
        else
            styleSheet.replace("%topbar_border_rgba%", colors.folderview.name());

        // --- apply -------------------------------------------------
        qApp->setStyleSheet(styleSheet);
    }
}
//------------------------------------------------------------------------------
void Settings::loadTheme() {
    if(useSystemColorScheme()) {
        setColorScheme(ThemeStore::colorScheme(ColorSchemes::COLORS_SYSTEM));
    } else {
        BaseColorScheme base;
        m_themeConf->beginGroup("Colors");
        for(const ThemeColorDesc &row : kThemeColors) {
            QColor c(m_themeConf->value(row.key, row.def).toString());
            if(!c.isValid())
                c = QColor(row.def); // junk from a hand-edited theme file
            base.*row.base = c;
        }
        base.tid = m_themeConf->value("tid", "-1").toInt();
        m_themeConf->endGroup();
        setColorScheme(ColorScheme(base));
    }
}
void Settings::saveTheme() {
    if(useSystemColorScheme())
        return;
    m_themeConf->beginGroup("Colors");
    for(const ThemeColorDesc &row : kThemeColors)
        m_themeConf->setValue(row.key, (m_colorScheme.*row.scheme).name());
    m_themeConf->setValue("tid", m_colorScheme.tid);
    m_themeConf->endGroup();
}
//------------------------------------------------------------------------------
const ColorScheme &Settings::colorScheme() {
    return m_colorScheme;
}
//------------------------------------------------------------------------------
void Settings::setColorScheme(ColorScheme scheme) {
    m_colorScheme = scheme;
    loadStylesheet();
}
//------------------------------------------------------------------------------
void Settings::setColorTid(int tid) {
    m_colorScheme.tid = tid;
}
