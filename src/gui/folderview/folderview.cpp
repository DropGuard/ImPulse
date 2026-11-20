#include "folderview.h"

#include <utility>
#include "ui_folderview.h"

FolderView::FolderView(QWidget *parent) : FloatingWidgetContainer(parent), m_ui(new Ui::FolderView) {
    m_ui->setupUi(this);

    // the grid scope's QShortcuts ride this surface and live and die with it
    actionManager->attachGridSurface(m_ui->thumbnailGrid);

    // ------- filesystem view --------
    QString style = "font: %1pt;";
    style = style.arg(QApplication::font().pointSize());
    m_ui->dirTreeView->setStyleSheet(style);

    m_optionsPopup = new FVOptionsPopup(this);
    m_popupTimerClutch.start();

    m_dirModel = new FileSystemModelCustom(this);
    m_dirModel->setFilter(QDir::NoDotAndDotDot | QDir::AllDirs);
    m_ui->dirTreeView->setModel(m_dirModel);

    QHeaderView *header = m_ui->dirTreeView->header();
    header->hideSection(1); // size
    header->hideSection(2); // type
    header->hideSection(3); // mod date

#ifdef _WIN32
    dirModel->setRootPath("");
#else
    m_dirModel->setRootPath(QDir::homePath());
    QModelIndex idx = m_dirModel->index(m_dirModel->rootPath());
    m_ui->dirTreeView->setRootIndex(idx);
#endif
    // -------------------------------
    m_ui->upButton->setAction("goUp");
    m_ui->upButton->setIconPath(":res/icons/common/buttons/panel/up16.png");
    m_ui->upButton->setTriggerMode(TriggerMode::ClickTrigger);
    m_ui->settingsButton->setAction("openSettings");
    m_ui->settingsButton->setIconPath(":res/icons/common/buttons/panel/settings16.png");
    m_ui->exitButton->setAction("exit");
    m_ui->exitButton->setIconPath(":res/icons/common/buttons/panel/close16.png");
    m_ui->exitButton->setIconOffset(-1, 0);
    m_ui->docViewButton->setAction("documentView");
    m_ui->docViewButton->setIconPath(":res/icons/common/buttons/panel/document-view20.png");
    m_ui->togglePlacesPanelButton->setCheckable(true);
    m_ui->togglePlacesPanelButton->setIconPath(":res/icons/common/buttons/panel/toggle-panel20.png");
    m_ui->togglePlacesPanelButton->setIconOffset(1, 0);

    m_ui->optionsPopupButton->setCheckable(true);
    m_ui->optionsPopupButton->setIconPath(":res/icons/common/buttons/panel/folderview20.png");

    m_ui->sortingComboBox->setIconPath(":res/icons/common/other/sorting-mode16.png");

    m_ui->homeButton->setIconPath(":res/icons/common/buttons/panel-small/home12.png");
    m_ui->rootButton->setIconPath(":res/icons/common/buttons/panel-small/root12.png");

    int min = m_ui->thumbnailGrid->THUMBNAIL_SIZE_MIN;
    int max = m_ui->thumbnailGrid->THUMBNAIL_SIZE_MAX;
    int step = m_ui->thumbnailGrid->ZOOM_STEP;

    m_ui->zoomSlider->setMinimum(min / step);
    m_ui->zoomSlider->setMaximum(max / step);
    m_ui->zoomSlider->setSingleStep(1);
    m_ui->zoomSlider->setPageStep(1);

    m_ui->splitter->setStretchFactor(1, 50);

    connect(m_ui->thumbnailGrid, &FolderGridView::thumbnailsRequested, this, &FolderView::thumbnailsRequested);
    connect(m_ui->thumbnailGrid, &FolderGridView::thumbnailSizeChanged, this, &FolderView::onThumbnailSizeChanged);
    connect(m_ui->thumbnailGrid, &FolderGridView::itemActivated, this, &FolderView::itemActivated);
    connect(m_ui->thumbnailGrid, &FolderGridView::draggedOut, this, &FolderView::draggedOut);
    connect(m_ui->thumbnailGrid, &FolderGridView::draggedOver, this, &FolderView::draggedOver);
    connect(m_ui->thumbnailGrid, &FolderGridView::droppedInto, this, &FolderView::droppedInto);

    connect(m_ui->homeButton, &IconButton::clicked, this, &FolderView::onHomeBtn);
    connect(m_ui->rootButton, &IconButton::clicked, this, &FolderView::onRootBtn);

    connect(m_ui->zoomSlider, &QSlider::valueChanged, this, &FolderView::onZoomSliderValueChanged);
    connect(m_ui->sortingComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &FolderView::onSortingSelected);
    connect(m_ui->togglePlacesPanelButton, &ActionButton::toggled, this, &FolderView::onPlacesPanelButtonChecked);

    connect(m_ui->optionsPopupButton, &IconButton::toggled, this, &FolderView::onOptionsPopupButtonToggled);
    connect(m_optionsPopup, &FVOptionsPopup::dismissed, this, &FolderView::onOptionsPopupDismissed);
    connect(m_optionsPopup, &FVOptionsPopup::viewModeSelected, this, &FolderView::onViewModeSelected);

    connect(m_ui->dirTreeView, &TreeViewCustom::droppedIn, this, &FolderView::onDroppedInByIndex);
    connect(m_ui->dirTreeView, &TreeViewCustom::tabbedOut, this, &FolderView::onTreeViewTabOut);

    m_ui->sortingComboBox->setItemDelegate(new QStyledItemDelegate(m_ui->sortingComboBox));
    m_ui->sortingComboBox->view()->setTextElideMode(Qt::ElideNone);

    connect(m_ui->splitter, &QSplitter::splitterMoved, this, &FolderView::onSplitterMoved);

    readSettings();

    QSizePolicy sp_retain = sizePolicy();
    sp_retain.setRetainSizeWhenHidden(true);
    setSizePolicy(sp_retain);
    connect(settings, &Settings::settingsChanged, this, &FolderView::readSettings);

    hide();
}

void FolderView::readSettings() {
    m_ui->thumbnailGrid->setThumbnailSize(settings->folderViewIconSize());
    m_ui->thumbnailGrid->setShowLabels((settings->folderViewMode() != FV_SIMPLE));
    m_ui->togglePlacesPanelButton->setChecked(settings->placesPanel());

    setPlacesPanel(settings->placesPanel());
    m_ui->dirTreeView->setVisible(settings->placesPanelTreeExpanded());

    QList<int> sizes;
    sizes << settings->placesPanelWidth() << 1;
    m_ui->splitter->setSizes(sizes);
}

void FolderView::onSplitterMoved() {
    settings->setPlacesPanelWidth(m_ui->placesPanel->width());
}

void FolderView::onPlacesPanelButtonChecked(bool mode) {
    setPlacesPanel(mode);
    settings->setPlacesPanel(mode);
}

void FolderView::setPlacesPanel(bool mode) {
    if(width() >= 600)
        m_ui->placesPanel->setVisible(mode);
}

void FolderView::toggleFilesystemView() {
    if(m_ui->dirTreeView->isVisible())
        m_ui->dirTreeView->hide();
    else
        m_ui->dirTreeView->show();
    settings->setPlacesPanelTreeExpanded(m_ui->dirTreeView->isVisible());
}

void FolderView::onTreeViewTabOut() {
    m_ui->thumbnailGrid->setFocus();
    // TODO: maybe add a focus change indication? a border blink or something
}

// TODO: ask what to do
void FolderView::onDroppedInByIndex(QList<QString> paths, QModelIndex index) {
    emit moveUrlsRequested(std::move(paths), m_dirModel->filePath(index));
}

void FolderView::onOptionsPopupButtonToggled(bool mode) {
    if(mode) {
        // Fixes popup being shown again right after dismissing it
        // by clicking on this toggle button.
        // This issue is only present on windows (different Qt::Popup behavior)
        if(m_popupTimerClutch.elapsed() < 10) {
            m_ui->optionsPopupButton->setChecked(false);
            return;
        }
        QPoint pos = m_ui->optionsPopupButton->geometry().bottomRight() - QPoint(m_optionsPopup->width(), 0);
        m_optionsPopup->showAt(mapToGlobal(pos));
    }
}

void FolderView::onOptionsPopupDismissed() {
    m_popupTimerClutch.start();
    m_ui->optionsPopupButton->setChecked(false);
}

void FolderView::onViewModeSelected(FolderViewMode mode) {
    settings->setFolderViewMode(mode);
    m_ui->thumbnailGrid->setShowLabels((settings->folderViewMode() != FV_SIMPLE));
    emit showFoldersChanged((mode == FV_EXT_FOLDERS));
}

void FolderView::onThumbnailSizeChanged(int newSize) {
    m_ui->zoomSlider->setValue(newSize / m_ui->thumbnailGrid->ZOOM_STEP);
    settings->setFolderViewIconSize(newSize);
}

void FolderView::onZoomSliderValueChanged(int value) {
    m_ui->thumbnailGrid->setThumbnailSize(value * m_ui->thumbnailGrid->ZOOM_STEP);
}

// changed by user via combobox
void FolderView::onSortingSelected(int mode) {
    emit sortingSelected(static_cast<SortingMode>(mode));
}

void FolderView::onSortingChanged(SortingMode mode) {
    m_ui->sortingComboBox->blockSignals(true);
    m_ui->sortingComboBox->setCurrentIndex(static_cast<int>(mode));
    m_ui->sortingComboBox->blockSignals(false);
}

void FolderView::gridSelectNext() {
    m_ui->thumbnailGrid->selectNext();
}

void FolderView::gridSelectPrev() {
    m_ui->thumbnailGrid->selectPrev();
}

void FolderView::gridSelectAbove() {
    m_ui->thumbnailGrid->selectAbove();
}

void FolderView::gridSelectBelow() {
    m_ui->thumbnailGrid->selectBelow();
}

void FolderView::gridPageUp() {
    m_ui->thumbnailGrid->pageUp();
}

void FolderView::gridPageDown() {
    m_ui->thumbnailGrid->pageDown();
}

void FolderView::gridSelectFirst() {
    m_ui->thumbnailGrid->selectFirst();
}

void FolderView::gridSelectLast() {
    m_ui->thumbnailGrid->selectLast();
}

void FolderView::gridSelectAll() {
    m_ui->thumbnailGrid->selectAll();
}

void FolderView::activateCurrent() {
    m_ui->thumbnailGrid->activateCurrent();
}

FolderView::~FolderView() {
    delete m_ui;
}

// probably unneeded
void FolderView::show() {
    QWidget::show();
    m_ui->thumbnailGrid->setFocus();
}

// probably unneeded
void FolderView::hide() {
    QWidget::hide();
    m_ui->thumbnailGrid->clearFocus();
}

void FolderView::onFullscreenModeChanged(bool mode) {
    m_ui->exitButton->setHidden(!mode);
    if(mode) // hide 2px spacer
        m_ui->panelRightEdgeSpacer->changeSize(0, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
    else // show spacer
        m_ui->panelRightEdgeSpacer->changeSize(2, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_ui->topBar->layout()->invalidate();
}

void FolderView::focusInEvent(QFocusEvent *event) {
    Q_UNUSED(event)
    m_ui->thumbnailGrid->setFocus();
}

void FolderView::populate(int count) {
    m_ui->thumbnailGrid->populate(count);
}

void FolderView::setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) {
    m_ui->thumbnailGrid->setThumbnail(pos, thumb);
}

void FolderView::select(QList<int> indices) {
    m_ui->thumbnailGrid->select(indices);
}

void FolderView::select(int index) {
    m_ui->thumbnailGrid->select(index);
}

QList<int> FolderView::selection() {
    return m_ui->thumbnailGrid->selection();
}

void FolderView::focusOn(int index) {
    m_ui->thumbnailGrid->focusOn(index);
}

void FolderView::focusOnSelection() {
    m_ui->thumbnailGrid->focusOnSelection();
}

void FolderView::onHomeBtn() {
    emit directorySelected(QDir::homePath());
}

void FolderView::onRootBtn() {
    emit directorySelected("/");
}

void FolderView::setDirectoryPath(QString path) {
#ifdef __linux
    if(path.startsWith(QDir::homePath())) {
        if(m_dirModel->rootPath() != QDir::homePath()) {
            m_dirModel->setRootPath(QDir::homePath());
            QModelIndex idx = m_dirModel->index(m_dirModel->rootPath());
            m_ui->dirTreeView->setRootIndex(idx);
        }
    } else {
        m_dirModel->setRootPath("/");
        QModelIndex idx = m_dirModel->index(m_dirModel->rootPath());
        m_ui->dirTreeView->setRootIndex(idx);
    }
#endif
    m_ui->pathLabel->setText(path);

    if(m_ui->dirTreeView->currentIndex().data() == path)
        return;

    QModelIndex targetIndex = m_dirModel->index(path);
    bool keepExpand = m_ui->dirTreeView->isExpanded(targetIndex);
    bool collapse = !m_ui->dirTreeView->isExpanded(m_ui->dirTreeView->currentIndex().parent());

    if(collapse)
        m_ui->dirTreeView->collapseAll();
    m_ui->dirTreeView->setCurrentIndex(targetIndex);

    if(keepExpand)
        m_ui->dirTreeView->expand(targetIndex);

    // ok, i'm done with this shit. none of the "solutions" work
    // just do scrollTo after a delay and hope that model is loaded by then
    // larger than ~150ms becomes too noticeable
    QTimer::singleShot(150, this, &FolderView::fsTreeScrollToCurrent);
}

void FolderView::fsTreeScrollToCurrent() {
    m_ui->dirTreeView->scrollTo(m_ui->dirTreeView->currentIndex());
}

void FolderView::onTreeViewClicked(QModelIndex index) {
    emit directorySelected(m_dirModel->fileInfo(index).absoluteFilePath());
}

void FolderView::addItem() {
    m_ui->thumbnailGrid->addItem();
}

void FolderView::insertItem(int index) {
    m_ui->thumbnailGrid->insertItem(index);
}

void FolderView::removeItem(int index) {
    m_ui->thumbnailGrid->removeItem(index);
}

void FolderView::reloadItem(int index) {
    m_ui->thumbnailGrid->reloadItem(index);
}

void FolderView::renameItem(int oldIndex, int newIndex, const QString &name) {
    m_ui->thumbnailGrid->renameItem(oldIndex, newIndex, name);
}

void FolderView::setDragHover(int index) {
    m_ui->thumbnailGrid->setDragHover(index);
}

// prevent passthrough to parent
void FolderView::wheelEvent(QWheelEvent *event) {
    event->accept();
}

void FolderView::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void FolderView::resizeEvent(QResizeEvent *event) {
    Q_UNUSED(event)
    if(width() < 600)
        m_ui->placesPanel->setVisible(false);
    else if(m_ui->togglePlacesPanelButton->isChecked())
        m_ui->placesPanel->setVisible(true);

    if(width() < 510) {
        m_ui->zoomSlider->setVisible(false);
        m_ui->zoomSliderSpacer->changeSize(0, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_ui->pathbarSpacer->changeSize(0, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_ui->topBar->layout()->invalidate();
    } else {
        m_ui->zoomSlider->setVisible(true);
        m_ui->zoomSliderSpacer->changeSize(3, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_ui->pathbarSpacer->changeSize(12, 20, QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        m_ui->topBar->layout()->invalidate();
    }
}
