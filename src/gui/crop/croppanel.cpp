#include "croppanel.h"
#include "ui_croppanel.h"

#include "components/actionmanager/actionmanager.h"
#include "gui/overlays/cropanswer.h"

CropPanel::CropPanel(CropOverlay *_overlay, QWidget *parent)
    : SidePanelWidget(parent), m_ui(new Ui::CropPanel), m_overlay(_overlay) {
    m_ui->setupUi(this);
    setFocusPolicy(Qt::NoFocus);

    m_ui->ARcomboBox->setItemDelegate(new QStyledItemDelegate(m_ui->ARcomboBox));
    m_ui->ARcomboBox->view()->setTextElideMode(Qt::ElideNone);

    m_ui->headerIcon->setIconPath(":/res/icons/common/other/image-crop48.png");

    m_ui->ARcomboBox->setIconPath(":res/icons/common/other/dropDownArrow.png");

    hide();

    if(settings->defaultCropAction() == ACTION_CROP)
        setFocusCropBtn();
    else
        setFocusCropSaveBtn();

    connect(m_ui->cropButton, &PushButtonFocusInd::rightPressed, this, &CropPanel::setFocusCropBtn);
    connect(m_ui->cropSaveButton, &PushButtonFocusInd::rightPressed, this, &CropPanel::setFocusCropSaveBtn);

    connect(m_ui->cancelButton, &QPushButton::clicked, this, &CropPanel::cancel);
    connect(m_ui->cropButton, &PushButtonFocusInd::clicked, this, &CropPanel::doCrop);
    connect(m_ui->cropSaveButton, &PushButtonFocusInd::clicked, this, &CropPanel::doCropSave);
    connect(m_ui->width, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(m_ui->height, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(m_ui->posX, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(m_ui->posY, &QSpinBox::valueChanged, this, &CropPanel::onSelectionChange);
    connect(m_ui->ARX, &QDoubleSpinBox::valueChanged, this, &CropPanel::onAspectRatioChange);
    connect(m_ui->ARY, &QDoubleSpinBox::valueChanged, this, &CropPanel::onAspectRatioChange);
    connect(m_ui->ARcomboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &CropPanel::onAspectRatioSelected);

    connect(m_overlay, &CropOverlay::selectionChanged, this, &CropPanel::onSelectionOutsideChange);
    connect(this, &CropPanel::selectionChanged, m_overlay, &CropOverlay::onSelectionOutsideChange);
    connect(m_overlay, &CropOverlay::escPressed, this, &CropPanel::cancel);
    // the keyboard's Apply is always the in-memory crop — the keyboard never
    // writes the disk; Crop && Save stays a button-only convenience
    connect(m_overlay, &CropOverlay::cropApply, this, &CropPanel::doCrop);
    connect(this, &CropPanel::selectAll, m_overlay, &CropOverlay::selectAll);

    // the answers' keys on their faces — the .ui text stays the base. The
    // save chord commits the pending crop before writing, so Crop && Save
    // and Ctrl+S are the same action whenever a selection stands.
    m_ui->cropButton->setText(m_ui->cropButton->text() + QStringLiteral("  [Enter]"));
    m_ui->cropSaveButton->setText(m_ui->cropSaveButton->text() + QStringLiteral("  [Ctrl+S]"));
    m_ui->cancelButton->setText(m_ui->cancelButton->text() + QStringLiteral("  [Esc]"));
}

CropPanel::~CropPanel() {
    delete m_ui;
}

void CropPanel::setImageRealSize(QSize sz) {
    m_ui->width->setMaximum(sz.width());
    m_ui->height->setMaximum(sz.height());
    m_realSize = sz;
    // reset to free mode on image change
    m_ui->ARcomboBox->setCurrentIndex(0);
    // update aspect ratio in input fields

    onAspectRatioSelected();
}

void CropPanel::doCrop() {
    QRect target(m_ui->posX->value(), m_ui->posY->value(), m_ui->width->value(), m_ui->height->value());
    if(target.width() > 0 && target.height() > 0 && target.size() != m_realSize)
        emit crop(target);
    else
        emit cancel();
}

void CropPanel::doCropSave() {
    QRect target(m_ui->posX->value(), m_ui->posY->value(), m_ui->width->value(), m_ui->height->value());
    if(target.width() > 0 && target.height() > 0 && target.size() != m_realSize)
        emit cropAndSave(target);
    else
        emit cancel();
}

// on user input
void CropPanel::onSelectionChange() {
    emit selectionChanged(QRect(m_ui->posX->value(), m_ui->posY->value(), m_ui->width->value(), m_ui->height->value()));
}

// the fields are Custom's own face — typing here IS the ratio, live. They
// keep their own values (the last custom ratio), never seeded by the
// presets: that seeding was what made Custom and Current Image twins.
void CropPanel::onAspectRatioChange() {
    if(m_ui->ARX->value() > 0 && m_ui->ARY->value() > 0)
        m_overlay->setAspectRatio(QPointF(m_ui->ARX->value(), m_ui->ARY->value()));
}

// 0 == free
// 1 == custom (the hand-typed ratio; its fields are visible only now)
// 2 == current image
// 3 == current screen
// 4 ...
void CropPanel::onAspectRatioSelected() {
    QPointF newAR(1, 1);

    int index = m_ui->ARcomboBox->currentIndex();
    m_ui->ARInputWidget->setVisible(index == 1);
    switch(index) {
    case 0: {
        m_overlay->setLockAspectRatio(false);
        if(m_realSize.height() != 0)
            newAR = QPointF(qreal(m_realSize.width()) / m_realSize.height(), 1.0);
        break;
    }
    case 1: {
        newAR = QPointF(m_ui->ARX->value(), m_ui->ARY->value());
        break;
    }
    case 2: {
        newAR = QPointF(qreal(m_realSize.width()) / m_realSize.height(), 1.0);
        break;
    }
    case 3: {
        QScreen *screen = nullptr;
        screen = QGuiApplication::screenAt(mapToGlobal(m_ui->ARcomboBox->geometry().topLeft()));
        if(!screen)
            screen = QGuiApplication::primaryScreen();
        newAR = QPointF(qreal(screen->geometry().width()) / screen->geometry().height(), 1.0);
        break;
    }
    case 4: {
        newAR = QPointF(1.0, 1.0);
        break;
    }
    case 5: {
        newAR = QPointF(4.0, 3.0);
        break;
    }
    case 6: {
        newAR = QPointF(16.0, 9.0);
        break;
    }
    default: // apply aspect ratio
    {
        break;
    }
    }

    if(index)
        m_overlay->setAspectRatio(newAR);
}

void CropPanel::setFocusCropBtn() {
    settings->setDefaultCropAction(ACTION_CROP);
    m_ui->cropSaveButton->setHighlighted(false);
    m_ui->cropButton->setHighlighted(true);
}

void CropPanel::setFocusCropSaveBtn() {
    settings->setDefaultCropAction(ACTION_CROP_SAVE);
    m_ui->cropSaveButton->setHighlighted(true);
    m_ui->cropButton->setHighlighted(false);
}

// update input box values
void CropPanel::onSelectionOutsideChange(QRect rect) {
    m_ui->width->blockSignals(true);
    m_ui->height->blockSignals(true);
    m_ui->posX->blockSignals(true);
    m_ui->posY->blockSignals(true);

    m_ui->width->setValue(rect.width());
    m_ui->height->setValue(rect.height());
    m_ui->posX->setValue(rect.left());
    m_ui->posY->setValue(rect.top());

    m_ui->width->blockSignals(false);
    m_ui->height->blockSignals(false);
    m_ui->posX->blockSignals(false);
    m_ui->posY->blockSignals(false);
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
    QTimer::singleShot(0, m_ui->width, SLOT(setFocus()));
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
    } else if(actionManager->keyNamesSubjectChangingAction(event)) {
        // same rule as the overlay: the walk would move the image out from
        // under the frame being edited. Consumed.
    }
    // keys the surface does not own were already fired by QShortcutMap before
    // delivery (zoom fine-tuning is the point of Framing) — the funnel has no
    // keyboard jurisdiction here
}

void CropPanel::wheelEvent(QWheelEvent *event) {
    event->accept();
}
