#pragma once

#include "core/Session.h"

#include <QString>
#include <QWidget>

class QComboBox;
class QListWidget;
class QPushButton;

namespace fr {

// Edits the clips of a timeline: add the source that is on screen or one dragged from the Project
// list, remove, reorder, and show the whole timeline in the active viewer. Every edit goes through
// the session, so it can be undone.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(Session& session, QWidget* parent = nullptr);

    // The drag format of the Project list: source ids, one per line (UTF-8).
    static constexpr const char* kSourceIdsMime = "application/x-kenaepro-source-ids";

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    QString currentTimelineId() const;
    void updateButtons();
    int dropRow(const QPoint& position) const;  // -1: after the last clip

    Session& session_;
    QComboBox* timelines_ = nullptr;
    QListWidget* clips_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* defaultButton_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* upButton_ = nullptr;
    QPushButton* downButton_ = nullptr;
    QPushButton* showButton_ = nullptr;
    bool refreshing_ = false;
};

}  // namespace fr
