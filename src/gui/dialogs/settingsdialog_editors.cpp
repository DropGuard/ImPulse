/*
 * The three sub-editors of the settings dialog: the color-scheme page, the
 * scripts list editor, and the shortcut-table editor with the scope tags it
 * carries on Qt::UserRole. Same class, partial implementation —
 * settingsdialog.cpp keeps the shell, the ctor wiring, and the read/save
 * spine that mirrors every widget.
 */

#include "settingsdialog.h"
#include "appversion.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <algorithm>

#include <utility>
#include "ui_settingsdialog.h"

// the shortcut table's scope tags, carried as Qt::UserRole on the action
// item. Section headers carry no tag, which is what every sweep uses to
// skip them: a header is prose about a scenario, not a binding.
// Function-local statics on purpose: a namespace-scope QString is a
// non-POD global with a static-init order, which is exactly what clazy's
// non-pod-global-static exists to name.
namespace {
const QString &scopeGlobal() {
    static const QString s = QStringLiteral("all");
    return s;
}
const QString &scopeGrid() {
    static const QString s = QStringLiteral("grid");
    return s;
}
const QString &scopeInspect() {
    static const QString s = QStringLiteral("inspect");
    return s;
}
} // namespace

//------------------------------------------------------------------------------
void SettingsDialog::readColorScheme() {
    auto colors = settings->colorScheme();
    setColorScheme(colors);
}

void SettingsDialog::setColorScheme(ColorScheme colors) {
    switch(colors.tid) {
    case COLORS_LIGHT: ui->themeSelectorComboBox->setCurrentIndex(3); break;
    case COLORS_BLACK: ui->themeSelectorComboBox->setCurrentIndex(0); break;
    case COLORS_DARK: ui->themeSelectorComboBox->setCurrentIndex(1); break;
    case COLORS_DARKBLUE: ui->themeSelectorComboBox->setCurrentIndex(2); break;
    default: ui->themeSelectorComboBox->setCurrentIndex(-1); break;
    }
    ui->colorSelectorAccent->setColor(colors.accent);
    ui->colorSelectorBackground->setColor(colors.background);
    ui->colorSelectorFullscreen->setColor(colors.background_fullscreen);
    ui->colorSelectorFolderview->setColor(colors.folderview);
    ui->colorSelectorFolderviewPanel->setColor(colors.folderview_topbar);
    ui->colorSelectorText->setColor(colors.text);
    ui->colorSelectorIcons->setColor(colors.icons);
    ui->colorSelectorWidget->setColor(colors.widget);
    ui->colorSelectorWidgetBorder->setColor(colors.widget_border);
    ui->colorSelectorOverlay->setColor(colors.overlay);
    ui->colorSelectorOverlayText->setColor(colors.overlay_text);
    ui->colorSelectorScrollbar->setColor(colors.scrollbar);
}

//------------------------------------------------------------------------------
void SettingsDialog::saveColorScheme() {
    BaseColorScheme base;
    base.accent = ui->colorSelectorAccent->color();
    base.background = ui->colorSelectorBackground->color();
    base.background_fullscreen = ui->colorSelectorFullscreen->color();
    base.folderview = ui->colorSelectorFolderview->color();
    base.folderview_topbar = ui->colorSelectorFolderviewPanel->color();
    base.text = ui->colorSelectorText->color();
    base.icons = ui->colorSelectorIcons->color();
    base.widget = ui->colorSelectorWidget->color();
    base.widget_border = ui->colorSelectorWidgetBorder->color();
    base.overlay = ui->colorSelectorOverlay->color();
    base.overlay_text = ui->colorSelectorOverlayText->color();
    base.scrollbar = ui->colorSelectorScrollbar->color();
    base.tid = settings->colorScheme().tid;
    settings->setColorScheme(ColorScheme(base));
}
//------------------------------------------------------------------------------
// the keymap page renders as scenarios: one section per scope, each opened
// by a header row stating when the scope is live. Inside a section rows are
// sorted by action name — auto-sort stays off, it would shred the sections.
void SettingsDialog::readShortcuts() {
    ui->shortcutsTableWidget->clearContents();
    ui->shortcutsTableWidget->setRowCount(0);
    ui->shortcutsTableWidget->setSortingEnabled(false);

    addSectionHeader(tr("Global — everywhere else"));
    QList<QPair<QString, QString>> rows;
    const QMap<QString, QString> shortcuts = actionManager->allShortcuts();
    for(auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it)
        rows.append({it.value(), it.key()});
    std::sort(rows.begin(), rows.end());
    for(const auto &row : rows)
        addShortcutToTable(row.first, row.second, scopeGlobal());

    addSectionHeader(tr("Folder view — while the thumbnail grid holds focus"));
    rows.clear();
    const QMap<QString, QString> gridShortcuts = actionManager->allGridShortcuts();
    for(auto it = gridShortcuts.constBegin(); it != gridShortcuts.constEnd(); ++it)
        rows.append({it.value(), it.key()});
    std::sort(rows.begin(), rows.end());
    for(const auto &row : rows)
        addShortcutToTable(row.first, row.second, scopeGrid());

    addSectionHeader(tr("Inspect — while fit original / free zoom is armed"));
    rows.clear();
    const QMap<QString, QString> inspectShortcuts = actionManager->allInspectShortcuts();
    for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it)
        rows.append({it.value(), it.key()});
    std::sort(rows.begin(), rows.end());
    for(const auto &row : rows)
        addShortcutToTable(row.first, row.second, scopeInspect());
}
//------------------------------------------------------------------------------
void SettingsDialog::readScripts() {
    ui->scriptsListWidget->clear();
    const QMap<QString, Script> scripts = scriptManager->allScripts();
    QMapIterator<QString, Script> i(scripts);
    while(i.hasNext()) {
        i.next();
        addScriptToList(i.key());
    }
}
//------------------------------------------------------------------------------
// does not check if the shortcut already there
void SettingsDialog::addScriptToList(const QString &name) {
    if(name.isEmpty())
        return;

    QListWidget *list = ui->scriptsListWidget;
    QListWidgetItem *nameItem = new QListWidgetItem(name);
    nameItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    list->insertItem(ui->scriptsListWidget->count(), nameItem);
    list->sortItems(Qt::AscendingOrder);
}
//------------------------------------------------------------------------------
void SettingsDialog::addScript() {
    ScriptEditorDialog w;
    if(w.exec()) {
        if(w.scriptName().isEmpty())
            return;
        scriptManager->addScript(w.scriptName(), w.script());
        readScripts();
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::editScript() {
    int row = ui->scriptsListWidget->currentRow();
    if(row >= 0) {
        QString name = ui->scriptsListWidget->currentItem()->text();
        editScript(name);
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::editScript(QListWidgetItem *item) {
    if(item) {
        editScript(item->text());
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::editScript(const QString &name) {
    ScriptEditorDialog w(name, scriptManager->getScript(name));
    if(w.exec()) {
        if(w.scriptName().isEmpty())
            return;
        scriptManager->addScript(w.scriptName(), w.script());
        readScripts();
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::removeScript() {
    int row = ui->scriptsListWidget->currentRow();
    if(row >= 0) {
        QString scriptName = ui->scriptsListWidget->currentItem()->text();
        delete ui->scriptsListWidget->takeItem(row);
        saveShortcuts();
        actionManager->removeAllShortcuts("s:" + scriptName);
        actionManager->removeAllGridShortcuts("s:" + scriptName);
        actionManager->removeAllInspectShortcuts("s:" + scriptName);
        readShortcuts();
        scriptManager->removeScript(scriptName);
    }
}
//------------------------------------------------------------------------------
// does not check if the shortcut already there
void SettingsDialog::addShortcutToTable(const QString &action, const QString &shortcut, const QString &scope) {
    if(action.isEmpty() || shortcut.isEmpty())
        return;

    ui->shortcutsTableWidget->setRowCount(ui->shortcutsTableWidget->rowCount() + 1);
    QTableWidgetItem *actionItem = new QTableWidgetItem(action);
    actionItem->setTextAlignment(Qt::AlignCenter);
    actionItem->setData(Qt::UserRole, scope); // which table this row saves into
    ui->shortcutsTableWidget->setItem(ui->shortcutsTableWidget->rowCount() - 1, 0, actionItem);
    QTableWidgetItem *shortcutItem = new QTableWidgetItem(shortcut);
    shortcutItem->setTextAlignment(Qt::AlignCenter);
    ui->shortcutsTableWidget->setItem(ui->shortcutsTableWidget->rowCount() - 1, 1, shortcutItem);
}

// a scenario opener, not a binding: no scope tag, so every sweep skips it
void SettingsDialog::addSectionHeader(const QString &title) {
    const int row = ui->shortcutsTableWidget->rowCount();
    ui->shortcutsTableWidget->setRowCount(row + 1);
    QTableWidgetItem *header = new QTableWidgetItem(title);
    QFont bold = header->font();
    bold.setBold(true);
    header->setFont(bold);
    header->setFlags(Qt::ItemIsEnabled);
    ui->shortcutsTableWidget->setItem(row, 0, header);
    ui->shortcutsTableWidget->setSpan(row, 0, 1, 2);
}

// the scope a row belongs to; empty for the section headers
QString SettingsDialog::rowScope(int row) const {
    QTableWidgetItem *actionItem = ui->shortcutsTableWidget->item(row, 0);
    if(!actionItem)
        return QString();
    return actionItem->data(Qt::UserRole).toString();
}

// where a freshly added binding lands: the section the user is looking at —
// a header row or no selection means the global table
QString SettingsDialog::selectedScope() const {
    const int row = ui->shortcutsTableWidget->currentRow();
    if(row < 0 || row >= ui->shortcutsTableWidget->rowCount())
        return scopeGlobal();
    const QString scope = rowScope(row);
    return scope.isEmpty() ? scopeGlobal() : scope;
}

void SettingsDialog::addShortcut() {
    const QString scope = selectedScope();
    ShortcutCreatorDialog w;
    if(!w.exec())
        return;
    // same key, same scope: a replacement. The same key in ANOTHER scope is
    // the whole point of scopes and survives. Rows shift on removal, so the
    // sweep re-examines the index it just removed at.
    for(int i = 0; i < ui->shortcutsTableWidget->rowCount(); i++) {
        QTableWidgetItem *shortcutItem = ui->shortcutsTableWidget->item(i, 1);
        if(shortcutItem && shortcutItem->text() == w.selectedShortcut() && rowScope(i) == scope)
            removeShortcutAt(i--);
    }
    addShortcutToTable(w.selectedAction(), w.selectedShortcut(), scope);
    // select
    auto items = ui->shortcutsTableWidget->findItems(w.selectedShortcut(), Qt::MatchExactly);
    if(items.count()) {
        int newRow = ui->shortcutsTableWidget->row(items.at(0));
        ui->shortcutsTableWidget->selectRow(newRow);
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::removeShortcutAt(int row) {
    if(row < 0 || row >= ui->shortcutsTableWidget->rowCount())
        return;
    if(rowScope(row).isEmpty())
        return; // a section header is not a binding
    ui->shortcutsTableWidget->removeRow(row);
}
//------------------------------------------------------------------------------
void SettingsDialog::editShortcut(int row) {
    if(row < 0 || row >= ui->shortcutsTableWidget->rowCount())
        return;
    const QString scope = rowScope(row);
    if(scope.isEmpty())
        return; // headers open a section, they are not editable bindings
    ShortcutCreatorDialog w;
    w.setWindowTitle(tr("Edit shortcut"));
    w.setAction(ui->shortcutsTableWidget->item(row, 0)->text());
    w.setShortcut(ui->shortcutsTableWidget->item(row, 1)->text());
    if(!w.exec())
        return;
    // remove itself
    removeShortcutAt(row);
    // remove anything we are replacing in the same scope
    for(int i = 0; i < ui->shortcutsTableWidget->rowCount(); i++) {
        QTableWidgetItem *shortcutItem = ui->shortcutsTableWidget->item(i, 1);
        if(shortcutItem && shortcutItem->text() == w.selectedShortcut() && rowScope(i) == scope)
            removeShortcutAt(i--);
    }
    // re-add
    addShortcutToTable(w.selectedAction(), w.selectedShortcut(), scope);
    // re-select
    auto items = ui->shortcutsTableWidget->findItems(w.selectedShortcut(), Qt::MatchExactly);
    if(items.count()) {
        int newRow = ui->shortcutsTableWidget->row(items.at(0));
        ui->shortcutsTableWidget->selectRow(newRow);
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::editShortcut() {
    editShortcut(ui->shortcutsTableWidget->currentRow());
}
//------------------------------------------------------------------------------
void SettingsDialog::removeShortcut() {
    removeShortcutAt(ui->shortcutsTableWidget->currentRow());
}
//------------------------------------------------------------------------------
// each row saves into the table its section stands for; headers carry no
// scope and are skipped
void SettingsDialog::saveShortcuts() {
    actionManager->removeAllShortcuts();
    actionManager->removeAllGridShortcuts();
    actionManager->removeAllInspectShortcuts();
    for(int i = 0; i < ui->shortcutsTableWidget->rowCount(); i++) {
        QTableWidgetItem *actionItem = ui->shortcutsTableWidget->item(i, 0);
        QTableWidgetItem *shortcutItem = ui->shortcutsTableWidget->item(i, 1);
        if(!actionItem || !shortcutItem)
            continue; // a header spans column 0 and has no shortcut twin
        const QString scope = rowScope(i);
        const QString shortcut = shortcutItem->text();
        const QString action = actionItem->text();
        if(scope == scopeGrid())
            actionManager->addGridShortcut(shortcut, action);
        else if(scope == scopeInspect())
            actionManager->addInspectShortcut(shortcut, action);
        else if(scope == scopeGlobal())
            actionManager->addShortcut(shortcut, action);
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::resetShortcuts() {
    actionManager->resetDefaults();
    readShortcuts();
}
