#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace fr {

class Preferences;

// Edits the application settings. OK writes them to the Preferences object it was given; the
// caller applies them afterwards.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(Preferences& preferences, QWidget* parent = nullptr);

private:
    void accept() override;

    Preferences& preferences_;
    QLineEdit* ffmpeg_ = nullptr;
    QSpinBox* cache_ = nullptr;
    QSpinBox* audioBuffer_ = nullptr;
    QDoubleSpinBox* sequenceRate_ = nullptr;
    QCheckBox* confirmClose_ = nullptr;
    QCheckBox* apiEnabled_ = nullptr;
    QSpinBox* apiPort_ = nullptr;
    QDoubleSpinBox* scrub_ = nullptr;
};

}  // namespace fr
