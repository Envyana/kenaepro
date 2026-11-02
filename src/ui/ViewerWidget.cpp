#include "ui/ViewerWidget.h"

#include "core/StrokeRenderer.h"

#include <QFontMetricsF>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace fr {
namespace {

constexpr double kPickTolerance = 0.03;   // in picture heights
constexpr double kEraserRadius = 0.03;    // in picture heights
const QColor kBackgrounds[] = {QColor(24, 24, 24), QColor(0, 0, 0), QColor(200, 200, 200),
                               QColor(255, 255, 255)};

Stroke::Type strokeTypeFor(ViewerWidget::Tool tool) {
    switch (tool) {
        case ViewerWidget::Tool::Arrow: return Stroke::Type::Arrow;
        case ViewerWidget::Tool::Ellipse: return Stroke::Type::Ellipse;
        case ViewerWidget::Tool::Rectangle: return Stroke::Type::Rectangle;
        case ViewerWidget::Tool::Text: return Stroke::Type::Text;
        default: break;
    }
    return Stroke::Type::Pencil;
}

}  // namespace

ViewerWidget::ViewerWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(320, 180);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);  // a click on the picture takes the keys back from a text box
}

void ViewerWidget::setImage(const QImage& image) {
    image_ = image;
    update();
}

void ViewerWidget::setStrokes(const QVector<Stroke>& strokes, bool editable) {
    strokes_ = strokes;
    editable_ = editable;
    selected_ = -1;
    moving_ = false;
    update();
}

void ViewerWidget::setGhostStrokes(const QVector<Stroke>& strokes) {
    ghostStrokes_ = strokes;
    update();
}

void ViewerWidget::setGhosts(const QVector<Ghost>& ghosts) {
    ghosts_ = ghosts;
    update();
}

void ViewerWidget::setOverlay(Overlay overlay) {
    overlay_ = overlay;
    update();
}

void ViewerWidget::setFlipped(bool flipped) {
    flipped_ = flipped;
    update();
}

void ViewerWidget::setFlippedVertically(bool flipped) {
    flippedV_ = flipped;
    update();
}

void ViewerWidget::setRotation(int quarterTurns) {
    rotation_ = ((quarterTurns % 4) + 4) % 4;
    update();
}

void ViewerWidget::cycleBackground() {
    background_ = (background_ + 1) % static_cast<int>(std::size(kBackgrounds));
    update();
}

void ViewerWidget::setActiveHighlight(bool active) {
    activeHighlight_ = active;
    update();
}

void ViewerWidget::endGestures() {
    drawingActive_ = false;
    dragging_ = false;
    erasing_ = false;
    erasePath_.clear();
    moving_ = false;
    laserActive_ = false;
    draggingDivider_ = false;
    scrubbing_ = false;
}

void ViewerWidget::setTool(Tool tool) {
    tool_ = tool;
    endGestures();  // switching tools abandons whatever was in progress
    selected_ = -1;
    applyCursor();
    update();
}

void ViewerWidget::applyCursor() {
    Qt::CursorShape shape = Qt::CrossCursor;
    if (tool_ == Tool::Navigate) shape = Qt::ArrowCursor;
    if (tool_ == Tool::Select) shape = Qt::PointingHandCursor;
    setCursor(shape);
}

void ViewerWidget::clearSelection() {
    selected_ = -1;
    moving_ = false;
    update();
}

double ViewerWidget::imageAspect(int side) const {
    const QImage& image = picture(side);
    if (image.isNull() || image.height() <= 0) return 1.0;
    return static_cast<double>(image.width()) / image.height();
}

void ViewerWidget::setBadges(const QString& a, const QString& b) {
    if (a == badgeA_ && b == badgeB_) return;
    badgeA_ = a;
    badgeB_ = b;
    update();
}

void ViewerWidget::setSplit(bool on, Qt::Orientation orientation) {
    split_ = on;
    splitOrientation_ = orientation;
    if (!on) {
        imageB_ = QImage();
        strokesB_.clear();
        side_ = 0;
    }
    endGestures();
    selected_ = -1;
    update();
}

void ViewerWidget::setSplitImage(const QImage& image) {
    imageB_ = image;
    update();
}

void ViewerWidget::setSplitStrokes(const QVector<Stroke>& strokes, bool editable) {
    strokesB_ = strokes;
    editableB_ = editable;
    if (side_ == 1) {
        selected_ = -1;
        moving_ = false;
    }
    update();
}

void ViewerWidget::setSplitPosition(double position) {
    splitPos_ = std::clamp(position, 0.02, 0.98);
    update();
}

ViewerWidget::ViewState ViewerWidget::viewState() const {
    ViewState state;
    state.zoom = zoom_;
    state.panX = width() > 0 ? pan_.x() / width() : 0.0;
    state.panY = height() > 0 ? pan_.y() / height() : 0.0;
    return state;
}

void ViewerWidget::setViewState(const ViewState& state) {
    zoom_ = std::clamp(state.zoom, 0.05, 64.0);
    pan_ = QPointF(state.panX * width(), state.panY * height());
    update();
}

void ViewerWidget::resetView() {
    zoom_ = 1.0;
    pan_ = QPointF();
    update();
    emit viewChanged();
}

QRectF ViewerWidget::imageRect() const {
    if (image_.isNull()) return {};
    // A quarter turn swaps the sides that have to fit.
    const bool sideways = rotation_ % 2 == 1;
    const double shownWidth = sideways ? image_.height() : image_.width();
    const double shownHeight = sideways ? image_.width() : image_.height();
    const double fit = std::min(width() / shownWidth, height() / shownHeight);
    const double scale = fit * zoom_;
    const QSizeF size(image_.width() * scale, image_.height() * scale);
    const QPointF centre(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
    return QRectF(centre.x() - size.width() / 2.0, centre.y() - size.height() / 2.0, size.width(),
                  size.height());
}

QTransform ViewerWidget::viewTransform() const {
    const QPointF c = imageRect().center();
    QTransform transform;
    transform.translate(c.x(), c.y());
    transform.rotate(90.0 * rotation_);
    transform.scale(flipped_ ? -1.0 : 1.0, flippedV_ ? -1.0 : 1.0);
    transform.translate(-c.x(), -c.y());
    return transform;
}

QRectF ViewerWidget::displayRect() const {
    return viewTransform().mapRect(imageRect());
}

QRectF ViewerWidget::pictureRect(int side) const {
    const QRectF a = imageRect();
    if (side != 1 || imageB_.isNull() || a.isEmpty()) return a;
    // B keeps its own shape, fitted inside A's rectangle.
    const double scale = std::min(a.width() / imageB_.width(), a.height() / imageB_.height());
    const QSizeF size(imageB_.width() * scale, imageB_.height() * scale);
    return QRectF(a.center().x() - size.width() / 2.0, a.center().y() - size.height() / 2.0, size.width(),
                  size.height());
}

QRectF ViewerWidget::sideRect(int side) const {
    const QRectF a = imageRect();
    if (!split_) return a;
    if (splitOrientation_ == Qt::Horizontal) {
        const double x = a.left() + a.width() * splitPos_;
        return side == 0 ? QRectF(a.left(), a.top(), x - a.left(), a.height())
                         : QRectF(x, a.top(), a.right() - x, a.height());
    }
    const double y = a.top() + a.height() * splitPos_;
    return side == 0 ? QRectF(a.left(), a.top(), a.width(), y - a.top())
                     : QRectF(a.left(), y, a.width(), a.bottom() - y);
}

int ViewerWidget::sideAt(const QPointF& widgetPos) const {
    if (!split_ || imageB_.isNull()) return 0;
    const QRectF a = imageRect();
    if (a.isEmpty()) return 0;
    const QPointF p = viewTransform().inverted().map(widgetPos);
    const double fraction = splitOrientation_ == Qt::Horizontal ? (p.x() - a.left()) / a.width()
                                                                : (p.y() - a.top()) / a.height();
    return fraction >= splitPos_ ? 1 : 0;
}

bool ViewerWidget::onDivider(const QPointF& widgetPos) const {
    if (!split_ || imageB_.isNull()) return false;
    const QRectF a = imageRect();
    if (a.isEmpty()) return false;
    QLineF line;
    if (splitOrientation_ == Qt::Horizontal) {
        const double x = a.left() + a.width() * splitPos_;
        line = QLineF(x, a.top(), x, a.bottom());
    } else {
        const double y = a.top() + a.height() * splitPos_;
        line = QLineF(a.left(), y, a.right(), y);
    }
    line = viewTransform().map(line);
    // Distance from the point to the segment, in screen pixels.
    const QPointF d = line.p2() - line.p1();
    const double length2 = d.x() * d.x() + d.y() * d.y();
    double t = length2 > 0.0 ? ((widgetPos.x() - line.x1()) * d.x() + (widgetPos.y() - line.y1()) * d.y()) / length2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return QLineF(widgetPos, line.p1() + d * t).length() <= 6.0;
}

QPointF ViewerWidget::toNormalised(const QPointF& widgetPos) const {
    const QRectF r = pictureRect(side_);
    if (r.isEmpty()) return {};
    // Back through the flips and the rotation into the picture's own coordinates.
    const QPointF p = viewTransform().inverted().map(widgetPos);
    const double x = std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0);
    const double y = std::clamp((p.y() - r.top()) / r.height(), 0.0, 1.0);
    return QPointF(x, y);
}

void ViewerWidget::paintGL() {
    QPainter painter(this);
    painter.fillRect(rect(), kBackgrounds[background_]);

    if (image_.isNull()) {
        painter.setPen(background_ >= 2 ? QColor(90, 90, 90) : QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("Drop a video, an image sequence or a .kpro project here\n"
                            "or use File \u2192 Import Media\u2026 (Ctrl+I)"));
    } else {
        const QRectF target = imageRect();
        const double pictureWidth = image_.width();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.save();
        painter.setTransform(viewTransform(), true);
        if (split_) painter.setClipRect(sideRect(0));
        painter.drawImage(target, image_);
        for (const Ghost& ghost : ghosts_) {
            painter.setOpacity(ghost.opacity);
            painter.drawImage(target, ghost.image);
        }
        painter.setOpacity(1.0);
        if (!ghostStrokes_.isEmpty()) {
            painter.setOpacity(0.35);
            paintStrokes(painter, ghostStrokes_, target, pictureWidth);
            painter.setOpacity(1.0);
        }
        const auto paintSide = [&](int side) {
            const QVector<Stroke>& strokes = strokesOf(side);
            const QRectF rect = pictureRect(side);
            const double width = picture(side).width();
            for (int i = 0; i < strokes.size(); ++i) {
                const bool selected = side == side_ && i == selected_;
                const Stroke shown = (selected && moving_) ? translatedStroke(strokes[i], moveDelta_) : strokes[i];
                paintStroke(painter, shown, rect, width, selected);
            }
            if (drawingActive_ && side == side_) paintStroke(painter, drawing_, rect, width);
        };
        paintSide(0);
        if (split_ && !imageB_.isNull()) {
            painter.setClipRect(sideRect(1));
            painter.fillRect(sideRect(1), kBackgrounds[background_]);
            painter.drawImage(pictureRect(1), imageB_);
            paintSide(1);
            painter.setClipping(false);
            // The divider, with a grip in its middle.
            const QRectF a = imageRect();
            QLineF line;
            if (splitOrientation_ == Qt::Horizontal) {
                const double x = a.left() + a.width() * splitPos_;
                line = QLineF(x, a.top(), x, a.bottom());
            } else {
                const double y = a.top() + a.height() * splitPos_;
                line = QLineF(a.left(), y, a.right(), y);
            }
            painter.setPen(QPen(QColor(255, 255, 255, 220), 2));
            painter.drawLine(line);
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(line.center(), 6.0, 6.0);
        }
        painter.restore();

        drawOverlay(painter, displayRect());

        if (laserActive_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 40, 40, 190));
            painter.drawEllipse(laserPos_, 9.0, 9.0);
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.drawEllipse(laserPos_, 3.0, 3.0);
        }
        // Which picture is which: "A" and "B" in the corners of their sides.
        if (split_ && !imageB_.isNull()) {
            const QRectF a = viewTransform().mapRect(sideRect(0));
            const QRectF b = viewTransform().mapRect(sideRect(1));
            drawBadge(painter, a.topLeft(), badgeA_, false);
            if (splitOrientation_ == Qt::Horizontal) {
                drawBadge(painter, b.topRight(), badgeB_, true);
            } else {
                drawBadge(painter, b.topLeft(), badgeB_, false);
            }
        }
        if (tool_ == Tool::Eraser) {
            const double radius = kEraserRadius * target.height();
            painter.setPen(QPen(QColor(255, 255, 255, 200), 1));
            painter.setBrush(QColor(0, 0, 0, 40));
            painter.drawEllipse(cursor_, radius, radius);
        }
    }

    if (!split_ || image_.isNull()) drawBadge(painter, QPointF(0, 0), badgeA_, false);

    if (activeHighlight_) {
        painter.setPen(QPen(QColor(80, 140, 255), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void ViewerWidget::drawBadge(QPainter& painter, const QPointF& corner, const QString& text, bool right) const {
    if (text.isEmpty()) return;
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const QSizeF size(metrics.horizontalAdvance(text) + 14.0, metrics.height() + 6.0);
    const QPointF topLeft = right ? QPointF(corner.x() - size.width() - 8.0, corner.y() + 8.0)
                                  : QPointF(corner.x() + 8.0, corner.y() + 8.0);
    const QRectF box(topLeft, size);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(box, 4.0, 4.0);
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(box, Qt::AlignCenter, text);
}

void ViewerWidget::drawOverlay(QPainter& painter, const QRectF& rect) const {
    if (overlay_ == Overlay::None) return;
    painter.setPen(QPen(QColor(255, 255, 255, 140), 1));
    painter.setBrush(Qt::NoBrush);
    const QPointF c = rect.center();
    const auto vertical = [&](double fraction) {
        const double x = rect.left() + rect.width() * fraction;
        painter.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    };
    const auto horizontal = [&](double fraction) {
        const double y = rect.top() + rect.height() * fraction;
        painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    };

    switch (overlay_) {
        case Overlay::Center:
            vertical(0.5);
            horizontal(0.5);
            break;
        case Overlay::Thirds:
            for (const double f : {1.0 / 3.0, 2.0 / 3.0}) {
                vertical(f);
                horizontal(f);
            }
            break;
        case Overlay::Phi:  // the golden ratio: 0.382 and 0.618
            for (const double f : {0.381966, 0.618034}) {
                vertical(f);
                horizontal(f);
            }
            break;
        case Overlay::DynamicSymmetry: {
            // Both diagonals, and from the two other corners of each a line that meets it at a
            // right angle (the "reciprocals").
            const QPointF tl = rect.topLeft();
            const QPointF tr = rect.topRight();
            const QPointF br = rect.bottomRight();
            const QPointF bl = rect.bottomLeft();
            const auto foot = [](const QPointF& a, const QPointF& b, const QPointF& p) {
                const QPointF ab = b - a;
                const double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) /
                                 (ab.x() * ab.x() + ab.y() * ab.y());
                return a + ab * t;
            };
            painter.drawLine(tl, br);
            painter.drawLine(tr, bl);
            painter.drawLine(tr, foot(tl, br, tr));
            painter.drawLine(bl, foot(tl, br, bl));
            painter.drawLine(tl, foot(tr, bl, tl));
            painter.drawLine(br, foot(tr, bl, br));
            break;
        }
        case Overlay::SafeArea:
            for (const double fraction : {0.9, 0.8}) {  // action safe, title safe
                const QSizeF s(rect.width() * fraction, rect.height() * fraction);
                painter.drawRect(QRectF(c.x() - s.width() / 2.0, c.y() - s.height() / 2.0, s.width(), s.height()));
            }
            break;
        case Overlay::None:
            break;
    }
}

void ViewerWidget::wheelEvent(QWheelEvent* event) {
    if (image_.isNull()) return;
    const double factor = std::pow(1.0015, event->angleDelta().y());
    const double next = std::clamp(zoom_ * factor, 0.05, 64.0);
    const double ratio = next / zoom_;

    // Keep the point under the cursor fixed while zooming.
    const QPointF anchor = event->position();
    const QPointF widgetCentre(width() / 2.0, height() / 2.0);
    const QPointF imageCentre = widgetCentre + pan_;
    const QPointF newCentre = anchor + (imageCentre - anchor) * ratio;
    pan_ = newCentre - widgetCentre;
    zoom_ = next;
    update();
    emit viewChanged();
    event->accept();
}

void ViewerWidget::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    side_ = sideAt(pos);  // before activated(), so the window knows which side was meant
    setFocus(Qt::MouseFocusReason);
    emit activated();
    if (event->button() == Qt::LeftButton && onDivider(pos)) {
        draggingDivider_ = true;
        return;
    }
    if (event->button() == Qt::RightButton) {
        scrubbing_ = true;
        scrubCarry_ = 0.0;
        lastMouse_ = pos;
        setCursor(Qt::SizeHorCursor);
        return;
    }
    const bool pans = event->button() == Qt::MiddleButton ||
                      (event->button() == Qt::LeftButton && tool_ == Tool::Navigate);
    if (pans) {
        dragging_ = true;
        lastMouse_ = pos;
        return;
    }
    if (event->button() != Qt::LeftButton || image_.isNull()) return;
    if (!viewTransform().mapRect(pictureRect(side_)).contains(pos)) {  // only draw on the picture
        if (tool_ == Tool::Select && selected_ >= 0) {  // a click beside the picture drops the selection
            selected_ = -1;
            update();
        }
        return;
    }
    const QPointF n = toNormalised(pos);
    if (tool_ != Tool::Navigate && tool_ != Tool::Laser && tool_ != Tool::Eyedropper) emit gestureStarted();

    switch (tool_) {
        case Tool::Text:
            emit textRequested(n);
            return;
        case Tool::Eyedropper: {
            const QImage& image = picture(side_);
            const int x = std::clamp(static_cast<int>(n.x() * (image.width() - 1) + 0.5), 0, image.width() - 1);
            const int y = std::clamp(static_cast<int>(n.y() * (image.height() - 1) + 0.5), 0, image.height() - 1);
            emit colorPicked(image.pixelColor(x, y));
            return;
        }
        case Tool::Laser:
            laserActive_ = true;
            laserPos_ = pos;
            update();
            return;
        case Tool::Eraser:
            if (!editableSide(side_)) return;
            erasing_ = true;
            erasePath_ = {n};
            update();
            return;
        case Tool::Select:
            if (!editableSide(side_)) return;
            selected_ = strokeAt(strokesOf(side_), n, imageAspect(side_), kPickTolerance);
            moving_ = selected_ >= 0;
            moveStart_ = n;
            moveDelta_ = QPointF();
            update();
            return;
        default:
            break;
    }

    drawing_ = Stroke();
    drawing_.type = strokeTypeFor(tool_);
    drawing_.color = strokeColor_;
    drawing_.color.setAlphaF(std::clamp(strokeOpacity_, 0.0, 1.0) * strokeColor_.alphaF());
    drawing_.width = strokeWidth_;
    if (drawing_.type == Stroke::Type::Rectangle || drawing_.type == Stroke::Type::Ellipse) {
        drawing_.fillOpacity = fillOpacity_;
    }
    drawing_.points = {n};
    drawingActive_ = true;
    pressPos_ = pos;
    lastMouse_ = pos;
    update();
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    cursor_ = pos;
    if (draggingDivider_) {
        const QRectF a = imageRect();
        const QPointF p = viewTransform().inverted().map(pos);
        setSplitPosition(splitOrientation_ == Qt::Horizontal ? (p.x() - a.left()) / a.width()
                                                             : (p.y() - a.top()) / a.height());
        return;
    }
    if (split_ && event->buttons() == Qt::NoButton) {  // show that the divider can be dragged
        if (onDivider(pos)) {
            const bool across = (splitOrientation_ == Qt::Horizontal) == (rotation_ % 2 == 0);
            setCursor(across ? Qt::SplitHCursor : Qt::SplitVCursor);
        } else {
            applyCursor();
        }
    }
    if (scrubbing_) {
        const double perFrame = scrubPixelsPerFrame_ / ((event->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0);
        scrubCarry_ += (pos.x() - lastMouse_.x()) / perFrame;
        lastMouse_ = pos;
        const int frames = static_cast<int>(scrubCarry_);  // towards zero, the rest carries over
        if (frames != 0) {
            scrubCarry_ -= frames;
            emit scrubbed(frames);
        }
        return;
    }
    if (dragging_) {
        pan_ += pos - lastMouse_;
        lastMouse_ = pos;
        update();
    } else if (drawingActive_) {
        updateDrawing(pos);
    } else if (erasing_) {
        erasePath_.push_back(toNormalised(pos));
        update();
    } else if (moving_) {
        moveDelta_ = toNormalised(pos) - moveStart_;
        update();
    } else if (laserActive_) {
        laserPos_ = pos;
        update();
    } else if (tool_ == Tool::Eraser) {
        update();  // the eraser's ring follows the pointer
    }
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (draggingDivider_ && event->button() == Qt::LeftButton) {
        draggingDivider_ = false;
        return;
    }
    if (scrubbing_ && event->button() == Qt::RightButton) {
        scrubbing_ = false;
        applyCursor();
        return;
    }
    if (dragging_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        dragging_ = false;
        emit viewChanged();
    }
    if (event->button() != Qt::LeftButton) return;

    if (drawingActive_) {
        updateDrawing(pos);
        finishDrawing(pos);
    }
    if (erasing_) {
        erasing_ = false;
        erasePath_.push_back(toNormalised(pos));
        const QVector<QPointF> path = erasePath_;
        erasePath_.clear();
        update();
        emit eraseFinished(path);
    }
    if (moving_) {
        moving_ = false;
        const QPointF delta = toNormalised(pos) - moveStart_;  // the release position counts, not only the last move
        const int index = selected_;
        moveDelta_ = QPointF();
        update();
        // A click with a little jitter is not a move; 3 screen pixels is the threshold.
        const QRectF rect = displayRect();
        if (index >= 0 && (std::fabs(delta.x()) * rect.width() > 3.0 || std::fabs(delta.y()) * rect.height() > 3.0)) {
            emit strokeMoved(index, delta);
        }
    }
    if (laserActive_) {
        laserActive_ = false;
        update();
    }
}

void ViewerWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && tool_ == Tool::Navigate) resetView();
}

void ViewerWidget::updateDrawing(const QPointF& widgetPos) {
    const QPointF n = toNormalised(widgetPos);
    if (drawing_.type == Stroke::Type::Pencil) {
        if (QLineF(lastMouse_, widgetPos).length() >= 1.5) {  // skip sub-pixel jitter
            drawing_.points.push_back(n);
            lastMouse_ = widgetPos;
        }
    } else if (drawing_.points.size() < 2) {
        drawing_.points.push_back(n);  // shapes keep a start point and the current point
    } else {
        drawing_.points[1] = n;
    }
    update();
}

void ViewerWidget::finishDrawing(const QPointF& widgetPos) {
    drawingActive_ = false;
    Stroke done = std::move(drawing_);
    drawing_ = Stroke();

    bool valid = false;
    if (done.type == Stroke::Type::Pencil) {
        valid = !done.points.isEmpty();
        if (done.points.size() == 1) done.points.push_back(done.points.first());  // a click is a dot
        if (smoothing_ > 0 && done.points.size() > 2) done.points = smoothPoints(done.points, smoothing_);
    } else {
        valid = done.points.size() == 2 && QLineF(pressPos_, widgetPos).length() >= 3.0;
    }
    if (valid) emit strokeFinished(done);
    update();
}

}  // namespace fr
