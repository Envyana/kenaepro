#include "ui/ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>

namespace fr {

ExportDialog::ExportDialog(const QString& suggestedName, const QString& ffmpegPath, bool hasAudio,
                           bool hasBookmarks, QWidget* parent)
    : QDialog(parent), ffmpegPath_(ffmpegPath), suggestedName_(suggestedName) {
    setWindowTitle(tr("Export"));
    form_ = new QFormLayout(this);

    kind_ = new QComboBox(this);
    kind_->addItem(tr("Video (ffmpeg)"), 0);
    kind_->addItem(tr("Image sequence"), 1);
    form_->addRow(tr("Type"), kind_);

    path_ = new QLineEdit(this);
    auto* browseButton = new QPushButton(tr("Browse…"), this);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browseButton);
    form_->addRow(tr("Output"), pathRow);

    extension_ = new QComboBox(this);
    form_->addRow(tr("Format"), extension_);

    codec_ = new QComboBox(this);
    codec_->addItem(tr("H.264"), QStringLiteral("libx264"));
    codec_->addItem(tr("ProRes 422 HQ"), QStringLiteral("prores_ks"));
    codec_->addItem(tr("VP9"), QStringLiteral("libvpx-vp9"));
    codec_->addItem(tr("MPEG-4"), QStringLiteral("mpeg4"));
    form_->addRow(tr("Codec"), codec_);

    baseName_ = new QLineEdit(suggestedName, this);
    form_->addRow(tr("File name"), baseName_);
    padding_ = new QSpinBox(this);
    padding_->setRange(1, 10);
    padding_->setValue(4);
    form_->addRow(tr("Digits"), padding_);
    numbering_ = new QComboBox(this);
    numbering_->addItem(tr("Start at 1"), static_cast<int>(ExportOptions::Numbering::OneBased));
    numbering_->addItem(tr("Start at 0"), static_cast<int>(ExportOptions::Numbering::ZeroBased));
    numbering_->addItem(tr("Match the source frame number"), static_cast<int>(ExportOptions::Numbering::MatchFrame));
    form_->addRow(tr("Numbering"), numbering_);

    frames_ = new QComboBox(this);
    frames_->addItem(tr("Everything"), static_cast<int>(ExportOptions::FrameSet::All));
    frames_->addItem(tr("The playback range"), static_cast<int>(ExportOptions::FrameSet::Range));
    frames_->addItem(tr("Bookmarked frames"), static_cast<int>(ExportOptions::FrameSet::Bookmarks));
    if (!hasBookmarks) {
        if (auto* model = qobject_cast<QStandardItemModel*>(frames_->model())) model->item(2)->setEnabled(false);
    }
    form_->addRow(tr("Frames"), frames_);

    width_ = new QSpinBox(this);
    width_->setRange(0, 16384);
    width_->setSpecialValueText(tr("Source size"));
    height_ = new QSpinBox(this);
    height_->setRange(0, 16384);
    height_->setSpecialValueText(tr("Source size"));
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(width_);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), this));
    sizeRow->addWidget(height_);
    form_->addRow(tr("Size"), sizeRow);

    annotations_ = new QCheckBox(tr("Draw the annotations onto the pictures"), this);
    annotations_->setChecked(true);
    form_->addRow(QString(), annotations_);
    audio_ = new QCheckBox(tr("Include the sound"), this);
    audio_->setChecked(hasAudio);
    audio_->setEnabled(hasAudio);
    form_->addRow(QString(), audio_);
    overwrite_ = new QCheckBox(tr("Replace files that already exist"), this);
    form_->addRow(QString(), overwrite_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form_->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, [this] { browse(); });
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] { kindChanged(); });
    kindChanged();
}

void ExportDialog::kindChanged() {
    const bool video = kind_->currentData().toInt() == 0;
    extension_->clear();
    const QStringList formats = video ? QStringList{QStringLiteral("mp4"), QStringLiteral("mov"),
                                                    QStringLiteral("webm"), QStringLiteral("mkv")}
                                      : QStringList{QStringLiteral("png"), QStringLiteral("jpg"),
                                                    QStringLiteral("tif"), QStringLiteral("bmp")};
    extension_->addItems(formats);

    form_->setRowVisible(codec_, video);  // video only
    form_->setRowVisible(audio_, video);
    form_->setRowVisible(baseName_, !video);  // image sequences only
    form_->setRowVisible(padding_, !video);
    form_->setRowVisible(numbering_, !video);

    path_->setText(video ? QDir::home().filePath(suggestedName_ + QStringLiteral(".mp4"))
                         : QDir::home().filePath(suggestedName_));
}

void ExportDialog::browse() {
    if (kind_->currentData().toInt() == 0) {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export Video"), path_->text(),
                                                          tr("Video (*.mp4 *.mov *.webm *.mkv)"));
        if (file.isEmpty()) return;
        path_->setText(file);
        const int index = extension_->findText(QFileInfo(file).suffix().toLower());
        if (index >= 0) extension_->setCurrentIndex(index);
    } else {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Export Folder"), path_->text());
        if (!folder.isEmpty()) path_->setText(folder);
    }
}

ExportOptions ExportDialog::options() const {
    ExportOptions o;
    o.kind = kind_->currentData().toInt() == 0 ? ExportOptions::Kind::Video : ExportOptions::Kind::ImageSequence;
    o.frameSet = static_cast<ExportOptions::FrameSet>(frames_->currentData().toInt());
    o.numbering = static_cast<ExportOptions::Numbering>(numbering_->currentData().toInt());
    o.outputPath = path_->text().trimmed();
    o.extension = extension_->currentText();
    o.videoCodec = codec_->currentData().toString();
    o.baseName = baseName_->text().trimmed();
    o.padding = padding_->value();
    o.width = width_->value();
    o.height = height_->value();
    o.includeAnnotations = annotations_->isChecked();
    o.includeAudio = audio_->isChecked();
    o.overwrite = overwrite_->isChecked();
    o.ffmpegPath = ffmpegPath_;
    if (o.kind == ExportOptions::Kind::Video) {
        // The container decides the path's extension and, for WebM, the codec.
        const QFileInfo info(o.outputPath);
        if (info.suffix().compare(o.extension, Qt::CaseInsensitive) != 0) {
            // "name.mov" with the mp4 container chosen becomes "name.mp4", not "name.mov.mp4".
            const QString stem = info.suffix().isEmpty() ? o.outputPath : o.outputPath.chopped(info.suffix().size() + 1);
            o.outputPath = stem + QLatin1Char('.') + o.extension;
        }
        if (o.extension == QLatin1String("webm")) o.videoCodec = QStringLiteral("libvpx-vp9");
    }
    return o;
}

void ExportDialog::accept() {
    const ExportOptions o = options();
    // The picture's size is not known here; the even-size rule can only be checked when it is set.
    const bool sized = o.width > 0 && o.height > 0;
    const QString problem = validateExport(o, sized ? QSize(o.width, o.height) : QSize(2, 2));
    if (!problem.isEmpty()) {
        QMessageBox::warning(this, tr("Export"), problem);
        return;
    }
    QDialog::accept();
}

}  // namespace fr
