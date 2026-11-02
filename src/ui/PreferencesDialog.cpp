#include "ui/PreferencesDialog.h"

#include "core/Preferences.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

namespace fr {

PreferencesDialog::PreferencesDialog(Preferences& preferences, QWidget* parent)
    : QDialog(parent), preferences_(preferences) {
    setWindowTitle(tr("Preferences"));
    auto* form = new QFormLayout(this);

    ffmpeg_ = new QLineEdit(preferences_.ffmpegPath(), this);
    ffmpeg_->setPlaceholderText(tr("Search the PATH"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    auto* ffmpegRow = new QHBoxLayout;
    ffmpegRow->addWidget(ffmpeg_, 1);
    ffmpegRow->addWidget(browse);
    form->addRow(tr("ffmpeg"), ffmpegRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Select the ffmpeg executable"));
        if (!file.isEmpty()) ffmpeg_->setText(file);
    });

    cache_ = new QSpinBox(this);
    cache_->setRange(64, 32768);
    cache_->setSingleStep(256);
    cache_->setToolTip(tr("Decoded frames kept in memory per viewer. A playback range that fits plays "
                          "smoothly from memory after the first pass (1080p: about 8 MB a frame)."));
    cache_->setSuffix(tr(" MB"));
    cache_->setValue(preferences_.cacheMegabytes());
    form->addRow(tr("Frame cache per viewer"), cache_);

    audioBuffer_ = new QSpinBox(this);
    audioBuffer_->setRange(20, 1000);
    audioBuffer_->setSuffix(tr(" ms"));
    audioBuffer_->setValue(preferences_.audioBufferMs());
    form->addRow(tr("Audio buffer"), audioBuffer_);

    sequenceRate_ = new QDoubleSpinBox(this);
    sequenceRate_->setRange(1.0, 240.0);
    sequenceRate_->setDecimals(3);
    sequenceRate_->setValue(preferences_.sequenceFrameRate());
    form->addRow(tr("Image sequence frame rate"), sequenceRate_);

    scrub_ = new QDoubleSpinBox(this);
    scrub_->setRange(1.0, 50.0);
    scrub_->setDecimals(1);
    scrub_->setSuffix(tr(" px per frame"));
    scrub_->setValue(preferences_.scrubPixelsPerFrame());
    scrub_->setToolTip(tr("Right-drag in the viewer scrubs. Fewer pixels per frame is faster; Shift is ten times faster."));
    form->addRow(tr("Mouse scrub speed"), scrub_);

    confirmClose_ = new QCheckBox(tr("Ask before closing a project with unsaved changes"), this);
    confirmClose_->setChecked(preferences_.confirmOnClose());
    form->addRow(QString(), confirmClose_);

    apiEnabled_ = new QCheckBox(tr("Listen for scripts and Blender on 127.0.0.1"), this);
    apiEnabled_->setChecked(preferences_.apiEnabled());
    form->addRow(tr("Client API"), apiEnabled_);
    apiPort_ = new QSpinBox(this);
    apiPort_->setRange(1, 65535);
    apiPort_->setValue(preferences_.apiPort());
    form->addRow(tr("Port"), apiPort_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PreferencesDialog::accept() {
    preferences_.setFfmpegPath(ffmpeg_->text().trimmed());
    preferences_.setCacheMegabytes(cache_->value());
    preferences_.setAudioBufferMs(audioBuffer_->value());
    preferences_.setSequenceFrameRate(sequenceRate_->value());
    preferences_.setConfirmOnClose(confirmClose_->isChecked());
    preferences_.setScrubPixelsPerFrame(scrub_->value());
    preferences_.setApi(apiEnabled_->isChecked(), static_cast<quint16>(apiPort_->value()));
    QDialog::accept();
}

}  // namespace fr
