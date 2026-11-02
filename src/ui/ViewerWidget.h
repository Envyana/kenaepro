#pragma once

#include "core/ProjectModel.h"

#include <QColor>
#include <QImage>
#include <QOpenGLWidget>
#include <QPointF>
#include <QTransform>
#include <QVector>

#include <algorithm>

class QPainter;

namespace fr {

// Shows one frame fitted to the widget, with zoom and pan, guides, ghost frames and drawings, and
// lets the user draw on it. The widget only reports what the user did (a finished stroke, an
// erase, a move, a picked colour); storing it, and undo, is the session's job.
//
// Mouse and tablet pen both arrive as mouse events (Qt translates pen input), so a pen works as a
// mouse; pressure is not used. Dragging with the right button scrubs through the frames.
//
// Flips and rotation only change how the picture is shown: drawings stay in the picture's own
// coordinates, so they turn with it.
class ViewerWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    enum class Overlay { None, Center, Thirds, Phi, DynamicSymmetry, SafeArea };
    // Navigate pans with the left button; the other tools use it. The middle button pans and the
    // wheel zooms with every tool.
    enum class Tool { Navigate, Pencil, Arrow, Ellipse, Rectangle, Text, Eraser, Select, Eyedropper, Laser };

    struct Ghost {
        QImage image;
        double opacity = 0.3;
    };
    // The view of a picture: zoom (1 = fit) and pan, as fractions of the widget's size.
    struct ViewState {
        double zoom = 1.0;
        double panX = 0.0;
        double panY = 0.0;
    };

    explicit ViewerWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    // `editable` is false when the strokes shown are held over from an earlier frame: they cannot be
    // selected or erased here.
    void setStrokes(const QVector<Stroke>& strokes, bool editable = true);
    void setGhostStrokes(const QVector<Stroke>& strokes);  // drawn faintly, e.g. neighbouring frames'
    void setGhosts(const QVector<Ghost>& ghosts);          // other frames laid over this one
    void setOverlay(Overlay overlay);
    Overlay overlay() const { return overlay_; }
    void setFlipped(bool flipped);  // mirrored left to right
    bool isFlipped() const { return flipped_; }
    void setFlippedVertically(bool flipped);  // upside down
    bool isFlippedVertically() const { return flippedV_; }
    void setRotation(int quarterTurns);  // clockwise, in quarter turns; kept in 0..3
    int rotation() const { return rotation_; }
    // Screen pixels of right-button drag per frame when scrubbing; Shift scrubs ten times faster.
    void setScrubPixelsPerFrame(double pixels) { scrubPixelsPerFrame_ = std::max(0.5, pixels); }
    void cycleBackground();
    void setActiveHighlight(bool active);
    // A label in the corner, such as "A  shot.mp4"; empty for none. In split mode the second label
    // goes on the B side.
    void setBadges(const QString& a, const QString& b = {});

    // Split mode (a wipe, as in Keyframe Pro): the second viewer's picture is shown past a divider,
    // left/right (Qt::Horizontal) or top/bottom (Qt::Vertical). Each side keeps its own drawings, and
    // a gesture applies to the side it starts on (pressedSide()). Drag the divider to move it.
    void setSplit(bool on, Qt::Orientation orientation = Qt::Horizontal);
    bool isSplit() const { return split_; }
    void setSplitImage(const QImage& image);
    void setSplitStrokes(const QVector<Stroke>& strokes, bool editable = true);
    double splitPosition() const { return splitPos_; }  // 0..1 across the picture
    void setSplitPosition(double position);
    int pressedSide() const { return side_; }  // 0 (A) or 1 (B): where the last press was
    int sideAt(const QPointF& widgetPos) const;  // 0 (A) or 1 (B) at a point; 0 when not split

    void setTool(Tool tool);
    Tool tool() const { return tool_; }
    void setStrokeColor(const QColor& color) { strokeColor_ = color; }
    QColor strokeColor() const { return strokeColor_; }
    // Width of new strokes, in pixels of the picture (not of the screen).
    void setStrokeWidth(double picturePixels) { strokeWidth_ = picturePixels; }
    double strokeWidth() const { return strokeWidth_; }
    void setStrokeOpacity(double opacity) { strokeOpacity_ = opacity; }  // 0..1
    double strokeOpacity() const { return strokeOpacity_; }
    void setFillOpacity(double opacity) { fillOpacity_ = opacity; }  // rectangles, ellipses, text boxes
    double fillOpacity() const { return fillOpacity_; }
    void setSmoothing(int level) { smoothing_ = level; }  // pencil lines: 0 off, 1 low, 2 high
    int smoothing() const { return smoothing_; }

    double imageAspect(int side = 0) const;  // picture width / height; 1 when there is no picture
    ViewState viewState() const;
    void setViewState(const ViewState& state);
    int selectedStroke() const { return selected_; }  // -1: none
    void clearSelection();

public slots:
    void resetView();

signals:
    void activated();     // the user pressed a mouse button here
    void gestureStarted();  // a drawing, text, erase or select gesture began: the frame must hold still
    void viewChanged();   // zoom or pan changed by the user
    // Points are normalised to the picture: (0,0) top-left, (1,1) bottom-right.
    void strokeFinished(const fr::Stroke& stroke);
    void textRequested(const QPointF& normalised);
    void eraseFinished(const QVector<QPointF>& path);
    void strokeMoved(int index, const QPointF& delta);  // normalised
    void colorPicked(const QColor& color);
    void scrubbed(int frames);  // right-button drag: move the playhead this many frames

protected:
    void paintGL() override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    // The picture's rectangle before flips and rotation, and the transform that turns and mirrors it
    // about its centre. displayRect() is where the picture really is on screen.
    QRectF imageRect() const;
    QTransform viewTransform() const;
    QRectF displayRect() const;
    QRectF pictureRect(int side) const;   // A: imageRect(); B: its picture fitted inside A's
    QRectF sideRect(int side) const;      // the part of A's rectangle that shows that side
    bool onDivider(const QPointF& widgetPos) const;
    const QImage& picture(int side) const { return side == 1 ? imageB_ : image_; }
    const QVector<Stroke>& strokesOf(int side) const { return side == 1 ? strokesB_ : strokes_; }
    bool editableSide(int side) const { return side == 1 ? editableB_ : editable_; }
    void drawBadge(QPainter& painter, const QPointF& corner, const QString& text, bool right) const;
    QPointF toNormalised(const QPointF& widgetPos) const;
    void updateDrawing(const QPointF& widgetPos);
    void finishDrawing(const QPointF& widgetPos);
    void drawOverlay(QPainter& painter, const QRectF& imageRect) const;
    void endGestures();
    void applyCursor();  // the current tool's cursor

    QImage image_;
    QVector<Stroke> strokes_;
    bool editable_ = true;
    QVector<Stroke> ghostStrokes_;
    QVector<Ghost> ghosts_;
    Overlay overlay_ = Overlay::None;
    bool flipped_ = false;
    bool flippedV_ = false;
    int rotation_ = 0;
    int background_ = 0;
    bool activeHighlight_ = false;
    double zoom_ = 1.0;  // 1.0 = fit to the widget
    QPointF pan_;        // offset of the picture's centre from the widget's centre, in pixels

    Tool tool_ = Tool::Navigate;
    QColor strokeColor_ = QColor(255, 48, 48);
    double strokeWidth_ = 6.0;
    double strokeOpacity_ = 1.0;
    double fillOpacity_ = 0.0;
    int smoothing_ = 0;

    bool dragging_ = false;       // panning
    bool drawingActive_ = false;  // a stroke is being drawn
    Stroke drawing_;
    QPointF pressPos_;
    QPointF lastMouse_;
    QPointF cursor_;

    bool erasing_ = false;
    QVector<QPointF> erasePath_;
    int selected_ = -1;
    bool moving_ = false;
    QPointF moveStart_;
    QPointF moveDelta_;
    bool laserActive_ = false;
    QPointF laserPos_;
    QString badgeA_;
    QString badgeB_;
    bool split_ = false;
    Qt::Orientation splitOrientation_ = Qt::Horizontal;
    double splitPos_ = 0.5;
    QImage imageB_;
    QVector<Stroke> strokesB_;
    bool editableB_ = true;
    int side_ = 0;
    bool draggingDivider_ = false;
    bool scrubbing_ = false;
    double scrubCarry_ = 0.0;  // drag that has not added up to a whole frame yet
    double scrubPixelsPerFrame_ = 8.0;
};

}  // namespace fr
