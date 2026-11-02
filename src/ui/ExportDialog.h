#pragma once

#include "core/Exporter.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;

namespace fr {

// Asks how to export what the active viewer shows: a video through ffmpeg, or a numbered image
// sequence. The options are checked when the dialog is accepted.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    // `suggestedName` seeds the file or base name; `hasAudio` and `hasBookmarks` enable the
    // matching choices.
    ExportDialog(const QString& suggestedName, const QString& ffmpegPath, bool hasAudio, bool hasBookmarks,
                 QWidget* parent = nullptr);

    ExportOptions options() const;

private:
    void kindChanged();
    void browse();
    void accept() override;

    QString ffmpegPath_;
    QString suggestedName_;
    QFormLayout* form_ = nullptr;
    QComboBox* kind_ = nullptr;
    QLineEdit* path_ = nullptr;
    QComboBox* extension_ = nullptr;
    QComboBox* codec_ = nullptr;
    QLineEdit* baseName_ = nullptr;
    QSpinBox* padding_ = nullptr;
    QComboBox* numbering_ = nullptr;
    QComboBox* frames_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* annotations_ = nullptr;
    QCheckBox* audio_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
};

}  // namespace fr
