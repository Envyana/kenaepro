// Drawing through the real widgets, with simulated mouse input. These need a window system with
// OpenGL (QOpenGLWidget), so each test skips itself when no context can be created, for
// instance under Qt's "offscreen" platform on macOS.
#include "core/Session.h"
#include "ui/BookmarkSlider.h"
#include "ui/MainWindow.h"
#include "ui/TimelinePanel.h"
#include "ui/ViewerArea.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QColor>
#include <QImage>
#include <QDir>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSlider>
#include <QTreeWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QtTest>

using namespace fr;
using Tool = ViewerWidget::Tool;

namespace {

QImage grayImage(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(90, 90, 90));
    return image;
}

// One left-button drag: press, move through `via`, release.
void drag(QWidget* widget, const QPoint& from, const QList<QPoint>& via, const QPoint& to) {
    QTest::mousePress(widget, Qt::LeftButton, Qt::NoModifier, from);
    for (const QPoint& p : via) QTest::mouseMove(widget, p);
    QTest::mouseRelease(widget, Qt::LeftButton, Qt::NoModifier, to);
}

bool near(double a, double b) {
    return qAbs(a - b) < 1e-9;
}

// A numbered PNG sequence of `count` frames starting at number 1; returns its first file.
QString writeSequence(const QString& dir, const QString& prefix, int count) {
    QDir().mkpath(dir);
    for (int n = 1; n <= count; ++n) {
        QImage image(32, 24, QImage::Format_RGB32);
        image.fill(qRgb(8 * n, 0, 0));
        if (!image.save(QDir(dir).filePath(prefix + QString::number(n).rightJustified(4, QLatin1Char('0')) +
                                           QStringLiteral(".png")))) {
            return {};
        }
    }
    return QDir(dir).filePath(prefix + QStringLiteral("0001.png"));
}

}  // namespace

class UiTests : public QObject {
    Q_OBJECT

private slots:
    // A 200x100 image in a 400x300 widget is drawn at 400x200 with its top edge at y = 50, so a
    // widget point (x, y) maps to (x / 400, (y - 50) / 200).
    void viewer_reports_normalised_points() {
        ViewerWidget viewer;
        viewer.setFixedSize(400, 300);
        viewer.setImage(grayImage(200, 100));
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        if (!viewer.isValid()) QSKIP("no OpenGL context on this platform");

        QVector<Stroke> strokes;
        connect(&viewer, &ViewerWidget::strokeFinished, &viewer,
                [&strokes](const Stroke& s) { strokes.push_back(s); });

        // Rectangle: two points, normalised, carrying the viewer's colour and width.
        viewer.setStrokeColor(QColor(10, 20, 30));
        viewer.setStrokeWidth(7.5);
        viewer.setTool(Tool::Rectangle);
        drag(&viewer, QPoint(100, 100), {QPoint(200, 150)}, QPoint(300, 200));
        QCOMPARE(strokes.size(), 1);
        QCOMPARE(strokes[0].type, Stroke::Type::Rectangle);
        QCOMPARE(strokes[0].points.size(), 2);
        QVERIFY(near(strokes[0].points[0].x(), 0.25) && near(strokes[0].points[0].y(), 0.25));
        QVERIFY(near(strokes[0].points[1].x(), 0.75) && near(strokes[0].points[1].y(), 0.75));
        QCOMPARE(strokes[0].color, QColor(10, 20, 30));
        QCOMPARE(strokes[0].width, 7.5);

        // Pencil: every moved-through point is kept.
        viewer.setTool(Tool::Pencil);
        drag(&viewer, QPoint(100, 100),
             {QPoint(120, 110), QPoint(140, 120), QPoint(160, 125)}, QPoint(180, 130));
        QCOMPARE(strokes.size(), 2);
        QCOMPARE(strokes[1].type, Stroke::Type::Pencil);
        QVERIFY(strokes[1].points.size() >= 4);

        // A click with the pencil is a dot (two identical points).
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QCOMPARE(strokes.size(), 3);
        QCOMPARE(strokes[2].points.size(), 2);
        QCOMPARE(strokes[2].points[0], strokes[2].points[1]);

        // Not a stroke: a drag that starts off the picture, a tiny shape drag, the move tool.
        viewer.setTool(Tool::Rectangle);
        drag(&viewer, QPoint(100, 20), {QPoint(150, 80)}, QPoint(200, 120));
        drag(&viewer, QPoint(100, 100), {}, QPoint(101, 101));
        viewer.setTool(Tool::Navigate);
        drag(&viewer, QPoint(100, 100), {QPoint(150, 150)}, QPoint(200, 200));
        QCOMPARE(strokes.size(), 3);
    }

    void viewer_asks_for_text_at_the_clicked_point() {
        ViewerWidget viewer;
        viewer.setFixedSize(400, 300);
        viewer.setImage(grayImage(200, 100));
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        if (!viewer.isValid()) QSKIP("no OpenGL context on this platform");

        QVector<QPointF> requests;
        connect(&viewer, &ViewerWidget::textRequested, &viewer,
                [&requests](const QPointF& p) { requests.push_back(p); });
        bool drewAStroke = false;
        connect(&viewer, &ViewerWidget::strokeFinished, &viewer, [&drewAStroke] { drewAStroke = true; });

        viewer.setTool(Tool::Text);
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
        QCOMPARE(requests.size(), 1);
        QVERIFY(near(requests[0].x(), 0.25) && near(requests[0].y(), 0.25));
        QVERIFY(!drewAStroke);
    }

    void window_draws_with_every_tool_and_undoes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.filePath(QStringLiteral("frame.png"));
        QVERIFY(grayImage(200, 100).save(png));

        Session session;
        MainWindow window(session);
        ViewerArea* area = window.findChild<ViewerArea*>();
        QVERIFY(area);
        ViewerWidget* viewer = area->viewer(0);  // findChild would not say which of the two comes first
        QVERIFY(viewer);
        viewer->setFixedSize(400, 300);
        const QString id = session.importFile(png);
        QVERIFY(session.activateSource(id));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (!viewer->isValid()) QSKIP("no OpenGL context on this platform");

        const auto useTool = [&window](const char* name) {
            QAction* action = window.findChild<QAction*>(QString::fromLatin1(name));
            if (!action) return false;
            action->trigger();
            return true;
        };
        const auto strokeTypes = [&session, &id] {
            QVector<Stroke::Type> types;
            if (const FrameAnnotation* a = session.project().findSource(id)->annotationAt(0)) {
                for (const Stroke& s : a->strokes) types.push_back(s.type);
            }
            return types;
        };

        QVERIFY(useTool("tool.rectangle"));
        drag(viewer, QPoint(240, 90), {QPoint(280, 130)}, QPoint(340, 170));
        QVERIFY(useTool("tool.ellipse"));
        drag(viewer, QPoint(50, 80), {QPoint(90, 110)}, QPoint(150, 150));
        QVERIFY(useTool("tool.arrow"));
        drag(viewer, QPoint(60, 230), {QPoint(120, 190)}, QPoint(200, 160));
        QVERIFY(useTool("tool.pencil"));
        drag(viewer, QPoint(220, 200), {QPoint(250, 215), QPoint(280, 195), QPoint(310, 215)},
             QPoint(340, 195));

        const QVector<Stroke::Type> expected = {Stroke::Type::Rectangle, Stroke::Type::Ellipse,
                                                Stroke::Type::Arrow, Stroke::Type::Pencil};
        QCOMPARE(strokeTypes(), expected);

        // For looking at the result by hand: FR_TEST_SCREENSHOT=/some/file.png ctest ...
        if (qEnvironmentVariableIsSet("FR_TEST_SCREENSHOT")) {
            window.grab().save(qEnvironmentVariable("FR_TEST_SCREENSHOT"));
        }

        QAction* undo = window.findChild<QAction*>(QStringLiteral("action.undo"));
        QAction* redo = window.findChild<QAction*>(QStringLiteral("action.redo"));
        QVERIFY(undo && redo);
        QVERIFY(undo->isEnabled());
        undo->trigger();
        QCOMPARE(strokeTypes().size(), 3);
        undo->trigger();
        QCOMPARE(strokeTypes().size(), 2);
        redo->trigger();
        QCOMPARE(strokeTypes().size(), 3);

        session.clearStrokes();
        QVERIFY(strokeTypes().isEmpty());
        undo->trigger();
        QCOMPARE(strokeTypes().size(), 3);  // the clear is undone, the one undone stroke stays gone
    }

    // Needs no OpenGL: the slider is an ordinary widget, drawn into an image.
    void slider_paints_frame_and_range_bookmarks() {
        BookmarkSlider slider;
        slider.setRange(0, 99);
        slider.resize(600, 40);
        slider.setMarks({50}, {RangeBookmark{10, 20}});

        const QImage image = slider.grab().toImage();
        const double scale = image.devicePixelRatio();  // 2 on a Retina screen
        const auto colorAt = [&](int value, int fromBottom) {
            return QColor(image.pixel(qRound(slider.xForValue(value) * scale),
                                      qRound((slider.height() - fromBottom) * scale)));
        };

        QCOMPARE(colorAt(50, 3), BookmarkSlider::frameColor());  // a tick for the frame bookmark
        QCOMPARE(colorAt(15, 3), BookmarkSlider::rangeColor());  // a bar across the range
        QVERIFY(colorAt(80, 3) != BookmarkSlider::frameColor());  // nothing where there is no bookmark
        QVERIFY(colorAt(80, 3) != BookmarkSlider::rangeColor());
        QCOMPARE(colorAt(50, 10), BookmarkSlider::frameColor());  // a tick is taller than a bar
        QVERIFY(colorAt(15, 10) != BookmarkSlider::rangeColor());

        slider.setMarks({}, {});  // clearing the bookmarks clears the marks
        const QImage cleared = slider.grab().toImage();
        QVERIFY(QColor(cleared.pixel(qRound(slider.xForValue(50) * scale),
                                     qRound((slider.height() - 3) * scale))) != BookmarkSlider::frameColor());
    }

    void window_bookmark_actions_edit_the_project_and_the_list() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.filePath(QStringLiteral("frame.png"));
        QVERIFY(grayImage(200, 100).save(png));

        Session session;
        MainWindow window(session);  // never shown, so no OpenGL context is needed
        const QString id = session.importFile(png);
        QVERIFY(session.activateSource(id));
        QListWidget* list = window.findChild<QListWidget*>(QStringLiteral("bookmarks.list"));
        QVERIFY(list);
        const auto trigger = [&window](const char* name) {
            QAction* action = window.findChild<QAction*>(QString::fromLatin1(name));
            if (!action) return false;
            action->trigger();
            return true;
        };
        const auto source = [&session, &id] { return session.project().findSource(id); };

        QVERIFY(trigger("action.toggleFrameBookmark"));
        QCOMPARE(source()->frameBookmarks, (QVector<int>{0}));
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Frame 1")));  // shown 1-based

        QVERIFY(trigger("action.toggleRangeBookmark"));
        QCOMPARE(source()->rangeBookmarks.size(), 1);
        QCOMPARE(list->count(), 2);
        QVERIFY(list->item(1)->text().contains(QStringLiteral("Range 1")));

        list->setCurrentRow(0);  // the frame bookmark
        QVERIFY(trigger("action.removeBookmark"));
        QVERIFY(source()->frameBookmarks.isEmpty());
        QCOMPARE(list->count(), 1);

        QVERIFY(trigger("action.clearBookmarks"));
        QCOMPARE(list->count(), 0);
        QAction* undo = window.findChild<QAction*>(QStringLiteral("action.undo"));
        QVERIFY(undo);
        undo->trigger();
        QCOMPARE(list->count(), 1);  // the range bookmark is back
    }

    // --- viewers, tools and the timeline panel -------------------------------------------------

    // No OpenGL needed: nothing is shown, only the layout state is looked at.
    void viewer_area_switches_layout() {
        ViewerArea area;
        QSplitter* splitter = area.findChild<QSplitter*>();
        QVERIFY(splitter);
        QVERIFY(area.viewer(0));
        QVERIFY(area.viewer(1));
        QVERIFY(area.viewer(0) != area.viewer(1));

        area.setLayoutMode(QStringLiteral("single"));
        QVERIFY(area.viewer(1)->isHidden());
        area.setLayoutMode(QStringLiteral("horizontal"));
        QVERIFY(!area.viewer(1)->isHidden());
        QCOMPARE(splitter->orientation(), Qt::Horizontal);
        area.setLayoutMode(QStringLiteral("vertical"));
        QVERIFY(!area.viewer(1)->isHidden());
        QCOMPARE(splitter->orientation(), Qt::Vertical);
        area.setLayoutMode(QStringLiteral("single"));
        QVERIFY(area.viewer(1)->isHidden());

        QSignalSpy activated(&area, &ViewerArea::viewerActivated);
        emit area.viewer(1)->activated();
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.first().first().toInt(), 1);
    }

    void viewer_flip_mirrors_the_drawn_points() {
        ViewerWidget viewer;
        viewer.setFixedSize(400, 300);
        viewer.setImage(grayImage(200, 100));
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        if (!viewer.isValid()) QSKIP("no OpenGL context on this platform");

        QVector<Stroke> strokes;
        connect(&viewer, &ViewerWidget::strokeFinished, &viewer,
                [&strokes](const Stroke& s) { strokes.push_back(s); });
        viewer.setTool(Tool::Rectangle);
        viewer.setFlipped(true);
        QVERIFY(viewer.isFlipped());
        drag(&viewer, QPoint(100, 100), {QPoint(200, 150)}, QPoint(300, 200));
        QCOMPARE(strokes.size(), 1);
        // On screen the drag ran from x=100 to x=300; in the picture that is 0.75 to 0.25.
        QVERIFY(near(strokes[0].points[0].x(), 0.75) && near(strokes[0].points[1].x(), 0.25));
        QVERIFY(near(strokes[0].points[0].y(), 0.25) && near(strokes[0].points[1].y(), 0.75));
    }

    void viewer_eraser_select_and_eyedropper_report() {
        ViewerWidget viewer;
        viewer.setFixedSize(400, 300);
        viewer.setImage(grayImage(200, 100));
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        if (!viewer.isValid()) QSKIP("no OpenGL context on this platform");

        Stroke rect;
        rect.type = Stroke::Type::Rectangle;
        rect.points = {QPointF(0.25, 0.25), QPointF(0.75, 0.75)};  // widget (100,100)-(300,200)
        viewer.setStrokes({rect}, true);

        // Eraser: reports the path it swept, normalised.
        QSignalSpy erased(&viewer, &ViewerWidget::eraseFinished);
        viewer.setTool(Tool::Eraser);
        drag(&viewer, QPoint(100, 150), {QPoint(120, 150)}, QPoint(140, 150));
        QCOMPARE(erased.count(), 1);
        const auto path = erased.first().first().value<QVector<QPointF>>();
        QVERIFY(path.size() >= 2);
        QVERIFY(near(path.first().x(), 0.25) && near(path.first().y(), 0.5));

        // Select: a click on the outline selects it, a drag then moves it by the dragged distance.
        QSignalSpy moved(&viewer, &ViewerWidget::strokeMoved);
        viewer.setTool(Tool::Select);
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(100, 150));
        QCOMPARE(viewer.selectedStroke(), 0);
        drag(&viewer, QPoint(100, 150), {QPoint(120, 150)}, QPoint(140, 150));
        QCOMPARE(moved.count(), 1);
        QCOMPARE(moved.first().at(0).toInt(), 0);
        const QPointF delta = moved.first().at(1).toPointF();
        QVERIFY(qAbs(delta.x() - 0.1) < 1e-6 && qAbs(delta.y()) < 1e-6);
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(10, 280));  // empty space
        QCOMPARE(viewer.selectedStroke(), -1);

        // Strokes held over from another frame are not editable: nothing to select.
        viewer.setStrokes({rect}, false);
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(100, 150));
        QCOMPARE(viewer.selectedStroke(), -1);

        // Eyedropper: reports the colour of the picture under the click.
        QSignalSpy picked(&viewer, &ViewerWidget::colorPicked);
        viewer.setTool(Tool::Eyedropper);
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QCOMPARE(picked.count(), 1);
        QCOMPARE(picked.first().first().value<QColor>(), QColor(90, 90, 90));
    }

    void window_layout_and_timeline_panel() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Session session;
        MainWindow window(session);  // never shown
        const QString idA = session.importSequence(writeSequence(dir.filePath(QStringLiteral("a")), QStringLiteral("a."), 10));
        const QString idB = session.importSequence(writeSequence(dir.filePath(QStringLiteral("b")), QStringLiteral("b."), 6));
        QVERIFY(!idA.isEmpty() && !idB.isEmpty());

        const auto button = [&window](const char* name) {
            return window.findChild<QPushButton*>(QString::fromLatin1(name));
        };
        QPushButton* newTimeline = button("timeline.new");
        QPushButton* add = button("timeline.add");
        QPushButton* show = button("timeline.show");
        QVERIFY(newTimeline && add && show);
        QListWidget* clips = window.findChild<QListWidget*>(QStringLiteral("timeline.clips"));
        QVERIFY(clips);

        QVERIFY(!add->isEnabled());  // no timeline yet
        newTimeline->click();
        QCOMPARE(session.project().timelines.size(), 1);
        QVERIFY(!add->isEnabled());  // a timeline, but nothing on screen to add

        QVERIFY(session.activateSource(idA));
        QVERIFY(add->isEnabled());
        add->click();
        QVERIFY(session.activateSource(idB));
        add->click();
        QCOMPARE(session.project().timelines.first().clips.size(), 2);
        QCOMPARE(clips->count(), 2);

        QVERIFY(show->isEnabled());
        show->click();
        QCOMPARE(session.viewerTargetKind(), QStringLiteral("timeline"));
        QCOMPARE(session.playback().frameCount(), 16);

        clips->setCurrentRow(1);
        button("timeline.up")->click();
        QCOMPARE(session.project().timelines.first().clips.first().sourceId, idB);
        button("timeline.remove")->click();  // the row that moved up and is still selected
        QCOMPARE(session.project().timelines.first().clips.size(), 1);

        // The layout actions drive the session, and the session drives the viewers.
        QAction* horizontal = window.findChild<QAction*>(QStringLiteral("layout.horizontal"));
        QVERIFY(horizontal);
        horizontal->trigger();
        QCOMPARE(session.viewerCount(), 2);
        ViewerArea* area = window.findChild<ViewerArea*>();
        QVERIFY(area);
        QVERIFY(!area->viewer(1)->isHidden());
        window.findChild<QAction*>(QStringLiteral("layout.single"))->trigger();
        QCOMPARE(session.viewerCount(), 1);
        QVERIFY(area->viewer(1)->isHidden());
    }

    // The reported bug: a source dragged from the Project list onto the Timeline panel did nothing.
    void project_source_drags_onto_timeline() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Session session;
        MainWindow window(session);
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QString idA = session.importSequence(writeSequence(dir.filePath(QStringLiteral("a")), QStringLiteral("a."), 4));
        const QString idB = session.importSequence(writeSequence(dir.filePath(QStringLiteral("b")), QStringLiteral("b."), 4));
        const QString idC = session.importSequence(writeSequence(dir.filePath(QStringLiteral("c")), QStringLiteral("c."), 4));
        QVERIFY(!idA.isEmpty() && !idB.isEmpty() && !idC.isEmpty());

        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("project.tree"));
        auto* clips = window.findChild<QListWidget*>(QStringLiteral("timeline.clips"));
        QVERIFY(tree && clips);
        QVERIFY(tree->dragEnabled());
        const auto dragOf = [tree](const QString& id) -> QMimeData* {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == id) {
                    return tree->model()->mimeData({tree->indexFromItem(*it)});
                }
            }
            return nullptr;
        };
        const QLatin1String mime(TimelinePanel::kSourceIdsMime);
        const auto dropAt = [clips](QMimeData* data, const QPoint& at) {
            QWidget* target = clips->viewport();
            QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &enter);
            const bool accepted = enter.isAccepted() && enter.dropAction() == Qt::CopyAction;
            QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &drop);
            delete data;
            return accepted && drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
        };
        const auto order = [&session] {
            QStringList ids;
            for (const Clip& clip : session.project().timelines.first().clips) ids << clip.sourceId;
            return ids;
        };

        // A drop with no timeline yet starts one.
        QMimeData* dataB = dragOf(idB);
        QVERIFY(dataB && dataB->hasFormat(mime));
        QVERIFY(dropAt(dataB, QPoint(5, 5)));
        QCOMPARE(session.project().timelines.size(), 1);
        QCOMPARE(order(), QStringList({idB}));
        QCOMPARE(clips->count(), 1);

        // Upper half of a row: before it. Lower half of a row: after it.
        QVERIFY(dropAt(dragOf(idA), clips->visualItemRect(clips->item(0)).topLeft() + QPoint(5, 1)));
        QCOMPARE(order(), QStringList({idA, idB}));
        QVERIFY(dropAt(dragOf(idC), clips->visualItemRect(clips->item(0)).bottomLeft() + QPoint(5, -1)));
        QCOMPARE(order(), QStringList({idA, idC, idB}));
        QCOMPARE(clips->currentRow(), 1);

        // The Project list keeps its sources (the drag is a copy), and other drags are refused.
        QCOMPARE(session.project().sources.size(), 3);
        int sourceItems = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const QString id = (*it)->data(0, Qt::UserRole).toString();
            if (id == idA || id == idB || id == idC) ++sourceItems;
        }
        QCOMPARE(sourceItems, 3);
        auto* text = new QMimeData;
        text->setText(QStringLiteral("hello"));
        QVERIFY(!dropAt(text, QPoint(5, 5)));
        QCOMPARE(order().size(), 3);
    }

    // The reported bug: a source dragged onto viewer B did nothing; only the context menu worked.
    // Also: Shift selects a run of sources, and the run drags in list order.
    void project_sources_drop_onto_viewers_and_multi_select() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Session session;
        MainWindow window(session);
        window.resize(1400, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QString idA = session.importSequence(writeSequence(dir.filePath(QStringLiteral("a")), QStringLiteral("a."), 4));
        const QString idB = session.importSequence(writeSequence(dir.filePath(QStringLiteral("b")), QStringLiteral("b."), 4));
        const QString idC = session.importSequence(writeSequence(dir.filePath(QStringLiteral("c")), QStringLiteral("c."), 4));
        QVERIFY(!idA.isEmpty() && !idB.isEmpty() && !idC.isEmpty());
        QVERIFY(session.activateSource(idA));

        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("project.tree"));
        QVERIFY(tree);
        QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
        const auto itemOf = [tree](const QString& id) -> QTreeWidgetItem* {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == id) return *it;
            }
            return nullptr;
        };
        QVERIFY(itemOf(idA) && itemOf(idC));

        // Click the first, Shift+click the last: all three, dragged in list order.
        const auto click = [tree](QTreeWidgetItem* item, Qt::KeyboardModifiers modifiers) {
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, modifiers, tree->visualItemRect(item).center());
        };
        click(itemOf(idA), Qt::NoModifier);
        click(itemOf(idC), Qt::ShiftModifier);
        QCOMPARE(tree->selectedItems().size(), 3);
        QModelIndexList picked;
        for (const QString& id : {idC, idA, idB}) picked << tree->indexFromItem(itemOf(id));  // picked out of order
        std::unique_ptr<QMimeData> run(tree->model()->mimeData(picked));
        QVERIFY(run);
        QCOMPARE(QString::fromUtf8(run->data(QLatin1String(TimelinePanel::kSourceIdsMime))),
                 QStringList({idA, idB, idC}).join(QLatin1Char('\n')));
        // Ctrl+click takes one out again.
        click(itemOf(idB), Qt::ControlModifier);
        QCOMPARE(tree->selectedItems().size(), 2);

        const auto dragOf = [tree, &itemOf](const QString& id) {
            return tree->model()->mimeData({tree->indexFromItem(itemOf(id))});
        };
        // A drop at a point of the window, as the windowing system delivers it to the main window.
        const auto dropAt = [&window](QMimeData* data, const QPoint& at) {
            QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &enter);
            QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &move);
            const bool accepted = move.isAccepted() && move.dropAction() == Qt::CopyAction;
            QDropEvent drop(QPointF(at), Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &drop);
            delete data;
            return accepted && drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
        };
        ViewerArea* area = window.findChild<ViewerArea*>();
        QVERIFY(area);
        const auto centreOf = [&window](QWidget* widget) { return widget->mapTo(&window, widget->rect().center()); };

        // Side by side: onto B shows it as B, A keeps its source.
        window.findChild<QAction*>(QStringLiteral("layout.horizontal"))->trigger();
        QCOMPARE(session.viewerCount(), 2);
        QTest::qWait(50);
        QVERIFY(dropAt(dragOf(idB), centreOf(area->viewer(1))));
        QCOMPARE(session.viewerTargetId(1), idB);
        QCOMPARE(session.viewerTargetId(0), idA);
        QVERIFY(dropAt(dragOf(idC), centreOf(area->viewer(0))));
        QCOMPARE(session.viewerTargetId(0), idC);
        QCOMPARE(session.viewerTargetId(1), idB);

        // Split A | B in one picture: the side of the divider decides.
        window.findChild<QAction*>(QStringLiteral("layout.split_horizontal"))->trigger();
        QTest::qWait(50);
        ViewerWidget* split = area->viewer(0);
        const QPoint rightSide = split->mapTo(&window, QPoint(split->width() * 9 / 10, split->height() / 2));
        QVERIFY(dropAt(dragOf(idA), rightSide));
        QCOMPARE(session.viewerTargetId(1), idA);
        QCOMPARE(session.viewerTargetId(0), idC);

        // Outside the viewers a source drop is refused, and the Project list keeps every source.
        QVERIFY(!dropAt(dragOf(idB), tree->mapTo(&window, tree->rect().center())));
        QCOMPARE(session.project().sources.size(), 3);

        // A file from the file manager dropped on B opens in B.
        window.findChild<QAction*>(QStringLiteral("layout.horizontal"))->trigger();
        QTest::qWait(50);
        const QString still = dir.filePath(QStringLiteral("still.png"));
        QVERIFY(grayImage(32, 24).save(still));
        auto* file = new QMimeData;
        file->setUrls({QUrl::fromLocalFile(still)});
        dropAt(file, centreOf(area->viewer(1)));  // enter, move and drop, as a real drag delivers them
        QCOMPARE(session.project().sources.size(), 4);
        QCOMPARE(session.sourcePath(session.viewerTargetId(1)), QFileInfo(still).absoluteFilePath());
        QCOMPARE(session.viewerTargetId(0), idC);
    }

    // The reported bug: with the cursor in Notes, Space typed into the note instead of stopping.
    void notes_space_stops_playback_and_typing_still_works() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 1; i <= 5; ++i) {
            QVERIFY(grayImage(64, 32).save(dir.filePath(QStringLiteral("shot.%1.png").arg(i, 4, 10, QLatin1Char('0')))));
        }
        Session session;
        MainWindow window(session);
        const QString id = session.importSequence(dir.filePath(QStringLiteral("shot.0001.png")));
        QVERIFY(!id.isEmpty());
        QVERIFY(session.activateSource(id));
        auto* notes = window.findChild<QPlainTextEdit*>();
        QVERIFY(notes);

        QCOMPARE(session.project().findSource(id)->name, QStringLiteral("shot.[0001-0005].png"));

        session.playback().play();
        QVERIFY(session.playback().isPlaying());
        QVERIFY(notes->isReadOnly());  // nothing can be typed onto a passing frame
        // Letters typed into the box are not shortcuts: I would have set the range start.
        notes->setFocus();
        const int rangeStart = session.playback().rangeStart();
        QTest::keyClick(notes, Qt::Key_I);
        QTest::keyClick(notes, Qt::Key_B);
        QCOMPARE(session.playback().rangeStart(), rangeStart);
        QVERIFY(session.project().findSource(id)->frameBookmarks.isEmpty());
        QTest::keyClick(notes, Qt::Key_Space);
        QVERIFY(!session.playback().isPlaying());
        QVERIFY(notes->toPlainText().isEmpty());

        QVERIFY(!notes->isReadOnly());
        QTest::keyClicks(notes, QStringLiteral("fix arm"));
        QCOMPARE(session.currentNote(), QStringLiteral("fix arm"));
    }

    void window_split_layout_marks_a_and_b() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.png"));
        const QString b = dir.filePath(QStringLiteral("b.png"));
        QVERIFY(grayImage(200, 100).save(a));
        QVERIFY(grayImage(200, 100).save(b));
        Session session;
        MainWindow window(session);
        const QString idA = session.importFile(a);
        const QString idB = session.importFile(b);
        QVERIFY(session.activateSource(idA));

        window.findChild<QAction*>(QStringLiteral("layout.split_horizontal"))->trigger();
        QCOMPARE(session.viewerCount(), 2);
        session.setActiveViewer(1);
        QVERIFY(session.activateSource(idB));
        ViewerArea* area = window.findChild<ViewerArea*>();
        QVERIFY(area->viewer(0)->isSplit());
        QVERIFY(area->viewer(1)->isHidden());

        // The project tree says which source is A and which is B.
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("project.tree"));
        QStringList letters;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if (!(*it)->text(1).isEmpty()) letters << (*it)->text(0) + QLatin1Char('=') + (*it)->text(1);
        }
        QVERIFY2(letters.contains(QStringLiteral("a.png=A")) && letters.contains(QStringLiteral("b.png=B")),
                 qPrintable(letters.join(QLatin1Char(','))));

        // Back to one viewer in one click.
        window.findChild<QAction*>(QStringLiteral("layout.single"))->trigger();
        QCOMPARE(session.viewerCount(), 1);
        QVERIFY(!area->viewer(0)->isSplit());
    }

    void window_volume_slider_sets_the_volume() {
        Session session;
        MainWindow window(session);
        auto* volume = window.findChild<QSlider*>(QStringLiteral("transport.volume"));
        QVERIFY(volume);
        volume->setValue(30);
        QVERIFY(qAbs(window.preferences().audioVolume() - 0.3) < 1e-6);
        window.findChild<QAction*>(QStringLiteral("action.volumeUp"))->trigger();
        QCOMPARE(volume->value(), 40);  // the key moves the slider too
        volume->setValue(100);
    }

    void window_ignores_drawing_without_a_source() {
        Session session;
        MainWindow window(session);
        ViewerArea* area = window.findChild<ViewerArea*>();
        QVERIFY(area);
        ViewerWidget* viewer = area->viewer(0);  // findChild would not say which of the two comes first
        QVERIFY(viewer);
        viewer->setFixedSize(400, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (!viewer->isValid()) QSKIP("no OpenGL context on this platform");

        window.findChild<QAction*>(QStringLiteral("tool.rectangle"))->trigger();
        drag(viewer, QPoint(100, 100), {QPoint(150, 150)}, QPoint(200, 200));
        QVERIFY(!session.undoStack().canUndo());
    }
};

QTEST_MAIN(UiTests)
#include "test_ui.moc"
