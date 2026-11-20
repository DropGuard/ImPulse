#include "resizedialog.h"
#include "ui_resizedialog.h"

ResizeDialog::ResizeDialog(QSize initialSize, QWidget *parent)
    : QDialog(parent), m_ui(new Ui::ResizeDialog), m_lastEdited(0) {
    m_ui->setupUi(this);
    setWindowModality(Qt::ApplicationModal);
    m_ui->percent->setFocus();

    this->m_originalSize = initialSize;
    m_targetSize = initialSize;

    m_ui->width->setValue(initialSize.width());
    m_ui->height->setValue(initialSize.height());

    m_ui->resetButton->setText(tr("Reset:") + " " + QString::number(initialSize.width()) + " x " +
                               QString::number(initialSize.height()));

    m_desktopSize = qApp->primaryScreen()->size();
    connect(m_ui->byPercentage, &QRadioButton::toggled, this, &ResizeDialog::onPercentageRadioButton);
    connect(m_ui->byAbsoluteSize, &QRadioButton::toggled, this, &ResizeDialog::onAbsoluteSizeRadioButton);
    connect(m_ui->percent, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ResizeDialog::percentChanged);
    connect(m_ui->width, qOverload<int>(&QSpinBox::valueChanged), this, &ResizeDialog::widthChanged);
    connect(m_ui->height, qOverload<int>(&QSpinBox::valueChanged), this, &ResizeDialog::heightChanged);
    connect(m_ui->keepAspectRatio, &QCheckBox::toggled, this, &ResizeDialog::onAspectRatioCheckbox);
    connect(m_ui->resComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ResizeDialog::setCommonResolution);
    connect(m_ui->fitDesktopButton, &QPushButton::pressed, this, &ResizeDialog::fitDesktop);
    connect(m_ui->fillDesktopButton, &QPushButton::pressed, this, &ResizeDialog::fillDesktop);
    connect(m_ui->resetButton, &QPushButton::pressed, this, &ResizeDialog::reset);
    connect(m_ui->cancelButton, &QPushButton::pressed, this, &ResizeDialog::reject);
    connect(m_ui->okButton, &QPushButton::pressed, this, &ResizeDialog::sizeSelect);
}

ResizeDialog::~ResizeDialog() {
    delete m_ui;
}

void ResizeDialog::sizeSelect() {
    if(m_targetSize != m_originalSize)
        emit sizeSelected(m_targetSize);
    this->accept();
}

void ResizeDialog::setCommonResolution(int index) {
    QSize res;
    switch(index) {
    case 1: res = QSize(1366, 768); break;
    case 2: res = QSize(1440, 900); break;
    case 3: res = QSize(1440, 1050); break;
    case 4: res = QSize(1600, 1200); break;
    case 5: res = QSize(1920, 1080); break;
    case 6: res = QSize(1920, 1200); break;
    case 7: res = QSize(2560, 1080); break;
    case 8: res = QSize(2560, 1440); break;
    case 9: res = QSize(2560, 1600); break;
    case 10: res = QSize(3840, 1600); break;
    case 11: res = QSize(3840, 2160); break;
    default: res = m_originalSize; break;
    }
    if(m_ui->keepAspectRatio->isChecked())
        m_targetSize = m_originalSize.scaled(res, Qt::KeepAspectRatio);
    else
        m_targetSize = m_originalSize.scaled(res, Qt::IgnoreAspectRatio);
    updateToTargetValues();
}

QSize ResizeDialog::newSize() {
    return m_targetSize;
}

void ResizeDialog::widthChanged(int newWidth) {
    m_lastEdited = 0;
    float factor = static_cast<float>(newWidth) / m_originalSize.width();
    m_targetSize.setWidth(newWidth);
    if(m_ui->keepAspectRatio->isChecked()) {
        m_targetSize.setHeight(static_cast<int>(m_originalSize.height() * factor));
    }
    updateToTargetValues();
}

void ResizeDialog::heightChanged(int newHeight) {
    m_lastEdited = 1;
    float factor = static_cast<float>(newHeight) / m_originalSize.height();
    m_targetSize.setHeight(newHeight);
    if(m_ui->keepAspectRatio->isChecked()) {
        m_targetSize.setWidth(static_cast<int>(m_originalSize.width() * factor));
    }
    updateToTargetValues();
}

void ResizeDialog::updateToTargetValues() {
    m_ui->width->blockSignals(true);
    m_ui->height->blockSignals(true);
    m_ui->width->setValue(m_targetSize.width());
    m_ui->height->setValue(m_targetSize.height());
    m_ui->width->blockSignals(false);
    m_ui->height->blockSignals(false);
}

void ResizeDialog::fitDesktop() {
    m_targetSize = m_originalSize.scaled(m_desktopSize, Qt::KeepAspectRatio);
    updateToTargetValues();
}

void ResizeDialog::fillDesktop() {
    m_targetSize = m_originalSize.scaled(m_desktopSize, Qt::KeepAspectRatioByExpanding);
    updateToTargetValues();
}

void ResizeDialog::onAspectRatioCheckbox() {
    resetResCheckBox();
    (m_lastEdited) ? heightChanged(m_ui->height->value()) : widthChanged(m_ui->width->value());
}

void ResizeDialog::onAbsoluteSizeRadioButton() {
    m_ui->width->blockSignals(true);
    m_ui->height->blockSignals(true);
    m_ui->percent->blockSignals(true);
    m_ui->keepAspectRatio->blockSignals(true);

    m_ui->width->setEnabled(true);
    m_ui->height->setEnabled(true);
    m_ui->percent->setEnabled(false);
    m_ui->keepAspectRatio->setEnabled(true);

    m_ui->width->blockSignals(false);
    m_ui->height->blockSignals(false);
    m_ui->percent->blockSignals(false);
    m_ui->keepAspectRatio->blockSignals(false);
}

void ResizeDialog::onPercentageRadioButton() {
    m_ui->width->blockSignals(true);
    m_ui->height->blockSignals(true);
    m_ui->percent->blockSignals(true);
    m_ui->keepAspectRatio->blockSignals(true);

    m_ui->width->setEnabled(false);
    m_ui->height->setEnabled(false);
    m_ui->percent->setEnabled(true);
    m_ui->keepAspectRatio->setChecked(true);
    m_ui->keepAspectRatio->setEnabled(false);
    percentChanged(m_ui->percent->value());

    m_ui->width->blockSignals(false);
    m_ui->height->blockSignals(false);
    m_ui->percent->blockSignals(false);
    m_ui->keepAspectRatio->blockSignals(false);
}

void ResizeDialog::resetResCheckBox() {
    m_ui->resComboBox->blockSignals(true);
    m_ui->resComboBox->setCurrentIndex(0);
    m_ui->resComboBox->blockSignals(false);
}

void ResizeDialog::percentChanged(double newPercent) {
    double scale = newPercent / 100.;
    m_targetSize.setWidth(m_originalSize.width() * scale);
    m_targetSize.setHeight(m_originalSize.height() * scale);

    updateToTargetValues();
}

void ResizeDialog::keyPressEvent(QKeyEvent *event) {
    if((event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)) {
        sizeSelect();
    } else if(event->key() == Qt::Key_Escape) {
        reject();
    } else {
        event->ignore();
    }
}

void ResizeDialog::reset() {
    resetResCheckBox();
    m_targetSize = m_originalSize;
    updateToTargetValues();
}

int ResizeDialog::exec() {
    resize(sizeHint());
    return QDialog::exec();
}
