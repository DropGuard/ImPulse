#include "croppanel.h"
#include "ui_croppanel.h"

#include "components/actionmanager/actionmanager.h"
#include "gui/overlays/cropanswer.h"

CropPanel::CropPanel(CropOverlay *_overlay, QWidget *parent)
    : SidePanelWidget(parent), ui(new Ui::CropPanel), overlay(_overlay) {
    ui->setupUi(this);
    setFocusPolicy(Qt::NoFocus);

    ui->ARcomboBox->setItemDelegate(new QStyledItemDelegate(ui->ARcomboBox));
    ui->ARcomboBox->view()->setTextElideMode(Qt::ElideNone);

    ui->headerIcon->setIconPath(":/res/icons/common/other/image-crop48.png");

    ui->ARcomboBox->setIconPath(":res/icons/common/other/dropDownArrow.png");

    hide();

    if(settings->defaultCropAction() == ACTION_CROP)
        setFocusCropBtn();
    else
        setFocusCropSaveBtn();

    connect(ui->cropButton, &PushButtonFocusInd::rightPressed, this, &CropPanel::setFocusCropBtn);
    connect(ui->cropSaveButton, &PushButtonFocusInd::rightPressed, this, &CropPanel::setFocusCropSaveBtn);

    connect(ui->cancelButton, &QPushButton::clicked, this, &CropPanel::cancel);
    connect(ui->cropButton, &PushButtonFocusInd::clicked, this, &CropPanel::doCrop);
    connect(ui->cropSaveButton, &PushButtonFocusInd::clicked, this, &CropPanel::doCropSave);
    connect(ui->width, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(ui->height, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(ui->posX, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(ui->posY, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(ui->ARcomboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &CropPanel::onAspectRatioSelected);

    connect(overlay, &CropOverlay::selectionChanged, this, &CropPanel::onSelectionOutsideChange);
    connect(this, &CropPanel::selectionChanged, overlay, &CropOverlay::onSelectionOutsideChange);
    connect(overlay, &CropOverlay::escPressed, this, &CropPanel::cancel);
    // the keyboard's Apply is always the in-memory crop — the keyboard never
    // writes the disk; Crop && Save stays a button-only convenience
    connect(overlay, &CropOverlay::cropApply, this, &CropPanel::doCrop);
    connect(this, &CropPanel::selectAll, overlay, &CropOverlay::selectAll);

    // the answers' keys on their faces — the .ui text stays the base. The
    // save chord commits the pending crop before writing, so Crop && Save
    // and Ctrl+S are the same action whenever a selection stands.
    ui->cropButton->setText(ui->cropButton->text() + QStringLiteral("  [Enter]"));
    ui->cropSaveButton->setText(ui->cropSaveButton->text() + QStringLiteral("  [Ctrl+S]"));
    ui->cancelButton->setText(ui->cancelButton->text() + QStringLiteral("  [Esc]"));
}

CropPanel::~CropPanel() {
    delete ui;
}

void CropPanel::setImageRealSize(QSize sz) {
    ui->width->setMaximum(sz.width());
    ui->height->setMaximum(sz.height());
    realSize = sz;
    // reset to free mode on image change
    ui->ARcomboBox->setCurrentIndex(0);
    // update aspect ratio in input fields

    onAspectRatioSelected();
}

void CropPanel::doCrop() {
    QRect target(ui->posX->value(), ui->posY->value(), ui->width->value(), ui->height->value());
    if(target.width() > 0 && target.height() > 0 && target.size() != realSize)
        emit crop(target);
    else
        emit cancel();
}

void CropPanel::doCropSave() {
    QRect target(ui->posX->value(), ui->posY->value(), ui->width->value(), ui->height->value());
    if(target.width() > 0 && target.height() > 0 && target.size() != realSize)
        emit cropAndSave(target);
    else
        emit cancel();
}

// on user input
void CropPanel::onSelectionChange() {
    emit selectionChanged(QRect(ui->posX->value(), ui->posY->value(), ui->width->value(), ui->height->value()));
}

// 0 == free
// 1 == current image
// 2 == current screen
// 3 ...
void CropPanel::onAspectRatioSelected() {
    QPointF newAR(1, 1);

    int index = ui->ARcomboBox->currentIndex();
    switch(index) {
    case 0: {
        overlay->setLockAspectRatio(false);
        if(realSize.height() != 0)
            newAR = QPointF(qreal(realSize.width()) / realSize.height(), 1.0);
        break;
    }
    case 1: {
        newAR = QPointF(qreal(realSize.width()) / realSize.height(), 1.0);
        break;
    }
    case 2: {
        QScreen *screen = nullptr;
        screen = QGuiApplication::screenAt(mapToGlobal(ui->ARcomboBox->geometry().topLeft()));
        if(!screen)
            screen = QGuiApplication::primaryScreen();
        newAR = QPointF(qreal(screen->geometry().width()) / screen->geometry().height(), 1.0);
        break;
    }
    case 3: {
        newAR = QPointF(1.0, 1.0);
        break;
    }
    case 4: {
        newAR = QPointF(4.0, 3.0);
        break;
    }
    case 5: {
        newAR = QPointF(16.0, 9.0);
        break;
    }
    default: // apply aspect ratio; update input fields
    {
        break;
    }
    }

    if(index)
        overlay->setAspectRatio(newAR);
}

void CropPanel::setFocusCropBtn() {
    settings->setDefaultCropAction(ACTION_CROP);
    ui->cropSaveButton->setHighlighted(false);
    ui->cropButton->setHighlighted(true);
}

void CropPanel::setFocusCropSaveBtn() {
    settings->setDefaultCropAction(ACTION_CROP_SAVE);
    ui->cropSaveButton->setHighlighted(true);
    ui->cropButton->setHighlighted(false);
}

// update input box values
void CropPanel::onSelectionOutsideChange(QRect rect) {
    ui->width->blockSignals(true);
    ui->height->blockSignals(true);
    ui->posX->blockSignals(true);
    ui->posY->blockSignals(true);

    ui->width->setValue(rect.width());
    ui->height->setValue(rect.height());
    ui->posX->setValue(rect.left());
    ui->posY->setValue(rect.top());

    ui->width->blockSignals(false);
    ui->height->blockSignals(false);
    ui->posX->blockSignals(false);
    ui->posY->blockSignals(false);
}

void CropPanel::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void CropPanel::show() {
    QWidget::show();
    // stackoverflow sorcery
    QTimer::singleShot(0, ui->width, SLOT(setFocus()));
}

void CropPanel::keyPressEvent(QKeyEvent *event) {
    event->accept();
    if(const std::optional<CropAnswer> answer = cropAnswerForKey(event)) {
        if(*answer == CropAnswer::Apply) {
            doCrop();
            return;
        }
        emit cancel();
        return;
    }
    if(event->matches(QKeySequence::SelectAll)) {
        emit selectAll();
    } else {
        // the overlay's idiom: keys the surface does not own stay live in
        // the action system (zoom fine-tuning is the point of Framing)
        actionManager->processEvent(event);
    }
}

void CropPanel::wheelEvent(QWheelEvent *event) {
    event->accept();
}
