#pragma once

#include "core/FrameNumbering.h"
#include "core/Preferences.h"
#include "core/Session.h"

#include <QAction>
#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QPointF>

#include <memory>
#include <utility>

class QCloseEvent;
class QComboBox;
class QFormLayout;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;

namespace fr {

class ApiServer;
class AudioController;
class BookmarkSlider;
class TimelinePanel;
class ViewerArea;
class ViewerWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Session& session, QWidget* parent = nullptr);
    ~MainWindow() override;

    // A ".kpro" path opens a project, a folder is imported, anything else is imported as media (a
    // numbered image becomes an image sequence) and shown.
    void openPath(const QString& path);
    // Starts the local Client API (Blender sync, scripts). False, with a reason, when the port is taken.
    bool startApi(quint16 port, QString* error = nullptr);
    void stopApi();
    Preferences& preferences() { return prefs_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // building
    void buildDocks();
    void buildMenus();
    void buildFileMenu();
    void buildEditMenu();
    void buildViewMenu();
    void buildSourceMenu();
    void buildPlaybackMenu();
    void buildAudioMenu();
    void buildTransport();
    void buildTools();
    void buildBookmarks();
    void buildInfo();
    void buildHelpMenu();
    void arrangeDocks();
    void addShortcutTooltips();
    // Customisable keys: every command in the menus and the drawing toolbar, with a stable id (its
    // object name, or its menu and text) under which a changed key is saved.
    struct Command {
        QString id;
        QString menu;
        QAction* action = nullptr;
    };
    QVector<Command> commands() const;
    void applyShortcutOverrides();
    void restoreLayout();

    template <typename Handler>
    QAction* addMenuAction(QMenu* menu, const char* id, const QString& label, const QKeySequence& key,
                           Handler&& handler) {
        QAction* action = menu->addAction(label);
        action->setObjectName(QStringLiteral("action.") + QLatin1String(id));
        if (!key.isEmpty()) action->setShortcut(key);
        connect(action, &QAction::triggered, this, std::forward<Handler>(handler));
        return action;
    }

    // refreshing what is on screen
    void refreshTree();
    void refreshViewer(int viewer);
    void refreshViews();
    void refreshNote();
    void refreshBookmarks();
    void refreshBadges();      // "A" / "B" in the viewers and in the project tree
    void refreshFrameLabel();  // the frame and time readouts under the viewer
    void refreshInfo();        // the Info panel, the status bar summary and the slider's marks
    void updateTitle();
    void updateStrokeStyle();  // colour, width, opacity, fill, smoothing: from the toolbar
    void applyViewState(int viewer);
    void storeView(int viewer);
    void syncSpeedCombo();
    void updateRecentMenu();

    // reacting
    void onFrameChanged(int frame);
    void onConfigured();
    void onNoteEdited();
    void onTreeActivated(QTreeWidgetItem* item);
    void onTreeContextMenu(const QPoint& position);
    void onStrokeFinished(int viewer, const Stroke& stroke);
    void onTextRequested(int viewer, const QPointF& normalised);
    void onColorPicked(const QColor& color);
    void onBookmarkActivated(QListWidgetItem* item);
    void removeSelectedBookmark();
    void setTool(int tool);

    // commands
    bool maybeSave();
    void openProject();
    void openProjectFile(const QString& path);
    bool saveProject();
    bool saveProjectAs();
    void importMedia();
    void importSequenceDialog();
    void importFolderDialog();
    void importPaths(const QStringList& paths, bool askAboutSequences);
    void activateFirstSource();
    void exportMedia();
    void saveFrameImage();
    void copyFrame();
    void showPreferences();
    void applyPreferences();
    void goToFrame();
    void customSpeed();
    void changeSpeed(double factor);
    void jumpToAnnotation(int direction);  // next (+1) or previous (-1) frame with drawings or a note
    void stepSeconds(int seconds);
    void showShortcuts();
    void setAlwaysOnTop(bool onTop);
    void resetLayout();
    void rotateViewers(int quarterTurns);
    // In a split layout both pictures are in the first viewer widget, so a widget and a session
    // viewer are not the same thing there.
    bool splitLayout() const;
    int viewerAt(const QPoint& windowPos) const;  // the session viewer under a point; -1 for none
    int sessionViewerFor(int widget) const;   // the session viewer a gesture in this widget meant
    ViewerWidget* widgetFor(int viewer) const;  // the widget that shows this session viewer
    void setVolumePercent(int percent);
    void showSourceInViewer(const QString& id, int viewer);
    void showError(const QString& title, const QString& message);

    Session& session_;
    Preferences prefs_;
    ViewerArea* area_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* noteEdit_ = nullptr;
    BookmarkSlider* slider_ = nullptr;       // the global timeline: every frame
    BookmarkSlider* rangeSlider_ = nullptr;  // the range timeline: only the playback range
    QWidget* globalRow_ = nullptr;
    QWidget* rangeRow_ = nullptr;
    QAction* steppedAction_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    QHash<QString, QKeySequence> defaultShortcuts_;
    bool layoutRestored_ = false;
    QLabel* frameCaption_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* rangeLabel_ = nullptr;
    QLabel* statusInfo_ = nullptr;
    struct InfoLabels {
        QLabel* name = nullptr;
        QLabel* file = nullptr;
        QLabel* resolution = nullptr;
        QLabel* fps = nullptr;
        QLabel* frames = nullptr;
        QLabel* duration = nullptr;
        QLabel* range = nullptr;
        QLabel* annotated = nullptr;
        QLabel* bookmarks = nullptr;
    } info_;
    QAction* playAction_ = nullptr;
    QComboBox* loopCombo_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QComboBox* colorCombo_ = nullptr;
    QComboBox* opacityCombo_ = nullptr;
    QComboBox* widthCombo_ = nullptr;
    QComboBox* fillCombo_ = nullptr;
    QComboBox* smoothCombo_ = nullptr;
    QListWidget* bookmarkList_ = nullptr;
    TimelinePanel* timelinePanel_ = nullptr;
    AudioController* audio_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* sourceMenu_ = nullptr;
    QMenu* layoutMenu_ = nullptr;
    QMenu* numberingMenu_ = nullptr;
    std::unique_ptr<ApiServer> api_;

    QVector<BookmarkEntry> bookmarkEntries_;
    NumberingMode numbering_ = NumberingMode::Global;
    int imageHeight_[2] = {0, 0};
    int ghostFrames_ = 0;
    double ghostOpacity_ = 0.35;
    bool ghostDrawings_ = false;
    QString lastSourceId_;
    bool dirty_ = false;
    bool updatingNote_ = false;
};

}  // namespace fr
