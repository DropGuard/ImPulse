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

// The shortcut table's scope identity rides each row as Qt::UserRole, using
// the SAME Scope enum ActionManager commits with (scopeTag / scopeFromTag are
// the codec's spelling). This file used to carry its own four function-local
// strings for the same concepts — a parallel vocabulary that had to be kept in
// sync by hand, and a save path whose if/else chain dropped an unknown tag in
// silence. Section headers carry no tag, which is what every sweep uses to
// skip them: a header is prose about a scenario, not a binding.

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

    // one section per scope, in kAllScopes order — the section title and the
    // table it saves into are the same Scope, so a fifth scope renders and
    // saves without this function learning its name.
    for(Scope scope : kAllScopes) {
        addSectionHeader(sectionTitle(scope));
        QList<QPair<QString, QString>> rows;
        const QMap<QString, QString> table = actionManager->tableForPublic(scope);
        for(auto it = table.constBegin(); it != table.constEnd(); ++it)
            rows.append({it.value(), it.key()});
        std::sort(rows.begin(), rows.end());
        for(const auto &row : rows)
            addShortcutToTable(row.first, row.second, scope);
        // the global section offers no orphans: an unbound action's home is a
        // view scope, and offering it in "everywhere else" would suggest a
        // meaning it does not have.
        if(scope != Scope::Global)
            addUnboundRows(scope);
    }
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
void SettingsDialog::addShortcutToTable(const QString &action, const QString &shortcut, Scope scope) {
    if(action.isEmpty() || shortcut.isEmpty())
        return;

    ui->shortcutsTableWidget->setRowCount(ui->shortcutsTableWidget->rowCount() + 1);
    QTableWidgetItem *actionItem = new QTableWidgetItem(action);
    actionItem->setTextAlignment(Qt::AlignCenter);
    actionItem->setData(Qt::UserRole, scopeTag(scope)); // which table this row saves into
    ui->shortcutsTableWidget->setItem(ui->shortcutsTableWidget->rowCount() - 1, 0, actionItem);
    QTableWidgetItem *shortcutItem = new QTableWidgetItem(shortcut);
    shortcutItem->setTextAlignment(Qt::AlignCenter);
    ui->shortcutsTableWidget->setItem(ui->shortcutsTableWidget->rowCount() - 1, 1, shortcutItem);
}

// an unbound action: registered and wired, but carrying no key anywhere.
// The empty shortcut cell is the message; gray text says the row is an
// invitation, not a binding. The scope tag still rides the action item, so
// editing the row binds into the section it sits in — placement IS the
// destination, the same "section = table" rule every bound row follows.
void SettingsDialog::addUnboundActionRow(const QString &action, Scope scope) {
    ui->shortcutsTableWidget->setRowCount(ui->shortcutsTableWidget->rowCount() + 1);
    QTableWidgetItem *actionItem = new QTableWidgetItem(action);
    actionItem->setTextAlignment(Qt::AlignCenter);
    actionItem->setData(Qt::UserRole, scopeTag(scope));
    actionItem->setForeground(QBrush(Qt::gray));
    ui->shortcutsTableWidget->setItem(ui->shortcutsTableWidget->rowCount() - 1, 0, actionItem);
    QTableWidgetItem *shortcutItem = new QTableWidgetItem();
    shortcutItem->setTextAlignment(Qt::AlignCenter);
    shortcutItem->setForeground(QBrush(Qt::gray));
    ui->shortcutsTableWidget->setItem(ui->shortcutsTableWidget->rowCount() - 1, 1, shortcutItem);
}

// The actions no table claims, offered at the bottom of the section their home
// scope names. The home is a property of the action, not of the editor: it is
// the row's `home` field (actions_table.h), so the 14 actions with no default
// key are each placed explicitly instead of 10 of them falling into a default
// branch here. The four-entry gridHome set this replaces was the same fact,
// written in the wrong file.
void SettingsDialog::addUnboundRows(Scope scope) {
    // every table read through the one mapping, and the maps iterated rather
    // than .values(): a temporary QList per scope is an allocation for nothing
    // and range-for over it detaches (clazy container-anti-pattern /
    // range-loop-detach)
    QSet<QString> bound;
    for(Scope s : kAllScopes) {
        const QMap<QString, QString> &table = actionManager->tableForPublic(s);
        for(auto it = table.constBegin(); it != table.constEnd(); ++it)
            bound.insert(it.value());
    }
    const QStringList registered = actionManager->actionList();
    for(const QString &action : registered) {
        if(bound.contains(action))
            continue;
        const Scope home = actionManager->homeScopeFor(action);
        if(home == scope)
            addUnboundActionRow(action, scope);
    }
}

// The section titles the keymap page renders — the editor's prose about WHEN
// a scope is live, distinct from scopeLabel's short name (which the conflict
// warning's "used for: ..." line needs). Keeping both here means a fifth
// scope adds one string, not a render pass.
QString SettingsDialog::sectionTitle(Scope scope) const {
    switch(scope) {
    case Scope::Global: return tr("Global — everywhere else");
    case Scope::Document: return tr("Document view — while one image is on screen");
    case Scope::Grid: return tr("Folder view — while the thumbnail grid holds focus");
    case Scope::Inspect: return tr("Inspect — while fit original / free zoom is armed");
    }
    return QString();
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

// the scope a row belongs to. False for the section headers (no tag), which
// is the "a header is prose, not a binding" rule every sweep rests on.
bool SettingsDialog::rowScope(int row, Scope &out) const {
    QTableWidgetItem *actionItem = ui->shortcutsTableWidget->item(row, 0);
    if(!actionItem)
        return false;
    const QString tag = actionItem->data(Qt::UserRole).toString();
    if(tag.isEmpty())
        return false;
    // a tag this build cannot name is a row from a NEWER config: treat it as
    // untagged (header-like) so the save path cannot drop it silently — the
    // unknown tag reaches the user as "not saved" only if a future scope's
    // rows actually reach here, and scopeFromTag is the one place to extend.
    return scopeFromTag(tag, out);
}

// where a freshly added binding lands: the section the user is looking at —
// a header row or no selection means the global table
Scope SettingsDialog::selectedScope() const {
    const int row = ui->shortcutsTableWidget->currentRow();
    Scope scope = Scope::Global;
    if(row >= 0 && row < ui->shortcutsTableWidget->rowCount() && rowScope(row, scope))
        return scope;
    return Scope::Global;
}

void SettingsDialog::addShortcut() {
    const Scope scope = selectedScope();
    ShortcutCreatorDialog w;
    if(!w.exec())
        return;
    // same key, same scope: a replacement. The same key in ANOTHER scope is
    // the whole point of scopes and survives. Rows shift on removal, so the
    // sweep re-examines the index it just removed at.
    for(int i = 0; i < ui->shortcutsTableWidget->rowCount(); i++) {
        QTableWidgetItem *shortcutItem = ui->shortcutsTableWidget->item(i, 1);
        Scope rowScopeValue = Scope::Global;
        if(shortcutItem && shortcutItem->text() == w.selectedShortcut() && rowScope(i, rowScopeValue) &&
           rowScopeValue == scope)
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
    Scope ignored;
    if(!rowScope(row, ignored))
        return; // a section header is not a binding
    ui->shortcutsTableWidget->removeRow(row);
}
//------------------------------------------------------------------------------
void SettingsDialog::editShortcut(int row) {
    if(row < 0 || row >= ui->shortcutsTableWidget->rowCount())
        return;
    Scope scope = Scope::Global;
    if(!rowScope(row, scope))
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
        Scope rowScopeValue = Scope::Global;
        if(shortcutItem && shortcutItem->text() == w.selectedShortcut() && rowScope(i, rowScopeValue) &&
           rowScopeValue == scope)
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
// Each row saves into the table its section stands for; headers carry no
// scope and are skipped. The four-way if/else this replaces named the scopes
// one at a time and had no else: a row whose tag it did not recognize was
// dropped in silence — the shape commit() exists to make unwritable, one
// layer up. A scope is now a value the row carries, and the save is one loop.
void SettingsDialog::saveShortcuts() {
    // The tables are replaced wholesale from the rendered rows, so the default
    // layer has to be re-applied first: a row the user deleted must not be
    // resurrected by the defaults that were merged into the map at load time.
    // (That is the in-memory form of "removed" — the persisted form is the
    // tombstone ActionManager writes; the two are separate facts and both
    // must hold across the dialog's save/close cycle.)
    for(Scope scope : kAllScopes)
        actionManager->removeAllShortcuts(scope);
    for(int i = 0; i < ui->shortcutsTableWidget->rowCount(); i++) {
        QTableWidgetItem *actionItem = ui->shortcutsTableWidget->item(i, 0);
        QTableWidgetItem *shortcutItem = ui->shortcutsTableWidget->item(i, 1);
        if(!actionItem || !shortcutItem)
            continue; // a header spans column 0 and has no shortcut twin
        Scope scope = Scope::Global;
        if(!rowScope(i, scope))
            continue; // a header: prose about a scenario, not a binding
        const QString shortcut = shortcutItem->text();
        if(shortcut.isEmpty())
            continue; // an unbound-action row: offered, never bound
        actionManager->insertShortcut(scope, shortcut, actionItem->text());
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::resetShortcuts() {
    actionManager->resetDefaults();
    readShortcuts();
}
