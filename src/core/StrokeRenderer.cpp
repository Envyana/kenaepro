#include "core/StrokeRenderer.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace fr {
namespace {

double segmentDistance(const QPointF& a, const QPointF& b, const QPointF& p) {
    const double abx = b.x() - a.x();
    const double aby = b.y() - a.y();
    const double length2 = abx * abx + aby * aby;
    if (length2 <= 1e-12) return std::hypot(p.x() - a.x(), p.y() - a.y());
    const double t = std::clamp(((p.x() - a.x()) * abx + (p.y() - a.y()) * aby) / length2, 0.0, 1.0);
    return std::hypot(p.x() - (a.x() + t * abx), p.y() - (a.y() + t * aby));
}

double rectOutlineDistance(const QRectF& r, const QPointF& p) {
    const QPointF tl = r.topLeft();
    const QPointF tr = r.topRight();
    const QPointF br = r.bottomRight();
    const QPointF bl = r.bottomLeft();
    return std::min(std::min(segmentDistance(tl, tr, p), segmentDistance(tr, br, p)),
                    std::min(segmentDistance(br, bl, p), segmentDistance(bl, tl, p)));
}

}  // namespace

void paintStroke(QPainter& painter, const Stroke& stroke, const QRectF& rect, double imageWidth,
                 bool highlighted) {
    if (stroke.points.isEmpty()) return;
    const auto map = [&rect](const QPointF& n) {
        return QPointF(rect.left() + n.x() * rect.width(), rect.top() + n.y() * rect.height());
    };
    const double scale = imageWidth > 0.0 ? rect.width() / imageWidth : 1.0;

    painter.save();
    QPen pen(stroke.color);
    pen.setWidthF(std::max(1.0, stroke.width * scale));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QBrush fill = Qt::NoBrush;
    if (stroke.fillOpacity > 0.0) {
        QColor color = stroke.color;
        color.setAlphaF(std::clamp(stroke.fillOpacity, 0.0, 1.0) * stroke.color.alphaF());
        fill = QBrush(color);
    }

    QRectF bounds;  // for the highlight
    const bool twoPoints = stroke.points.size() >= 2;
    switch (stroke.type) {
        case Stroke::Type::Pencil: {
            QPolygonF line;
            for (const QPointF& p : stroke.points) line << map(p);
            painter.drawPolyline(line);
            bounds = line.boundingRect();
            break;
        }
        case Stroke::Type::Rectangle:
            if (twoPoints) {
                bounds = QRectF(map(stroke.points.first()), map(stroke.points.last())).normalized();
                painter.setBrush(fill);
                painter.drawRect(bounds);
            }
            break;
        case Stroke::Type::Ellipse:
            if (twoPoints) {
                bounds = QRectF(map(stroke.points.first()), map(stroke.points.last())).normalized();
                painter.setBrush(fill);
                painter.drawEllipse(bounds);
            }
            break;
        case Stroke::Type::Arrow:
            if (twoPoints) {
                const QPointF from = map(stroke.points.first());
                const QPointF to = map(stroke.points.last());
                painter.drawLine(from, to);
                const double angle = std::atan2(to.y() - from.y(), to.x() - from.x());
                const double head = std::max(10.0, pen.widthF() * 4.0);
                for (const double offset : {2.6, -2.6}) {
                    painter.drawLine(to, to + QPointF(std::cos(angle + offset) * head,
                                                      std::sin(angle + offset) * head));
                }
                bounds = QRectF(from, to).normalized();
            }
            break;
        case Stroke::Type::Text: {
            QFont font = painter.font();
            font.setPixelSize(std::max(10, static_cast<int>(stroke.width * scale * 4.0)));
            painter.setFont(font);
            const QPointF at = map(stroke.points.first());
            bounds = QFontMetricsF(font).boundingRect(stroke.text).translated(at);
            if (fill != Qt::NoBrush) painter.fillRect(bounds.adjusted(-4, -2, 4, 2), fill);
            painter.drawText(at, stroke.text);
            break;
        }
    }

    if (highlighted && !bounds.isNull()) {
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds.adjusted(-6, -6, 6, 6));
    }
    painter.restore();
}

void paintStrokes(QPainter& painter, const QVector<Stroke>& strokes, const QRectF& imageRect,
                  double imageWidth) {
    for (const Stroke& stroke : strokes) paintStroke(painter, stroke, imageRect, imageWidth);
}

double distanceToStroke(const Stroke& stroke, const QPointF& point, double aspect) {
    constexpr double kFar = 1e9;
    if (stroke.points.isEmpty()) return kFar;
    const auto scaled = [aspect](const QPointF& q) { return QPointF(q.x() * aspect, q.y()); };
    const QPointF p = scaled(point);

    switch (stroke.type) {
        case Stroke::Type::Pencil: {
            if (stroke.points.size() == 1) {
                const QPointF a = scaled(stroke.points.first());
                return std::hypot(p.x() - a.x(), p.y() - a.y());
            }
            double best = kFar;
            for (int i = 0; i + 1 < stroke.points.size(); ++i) {
                best = std::min(best, segmentDistance(scaled(stroke.points[i]), scaled(stroke.points[i + 1]), p));
            }
            return best;
        }
        case Stroke::Type::Arrow:
            if (stroke.points.size() < 2) return kFar;
            return segmentDistance(scaled(stroke.points.first()), scaled(stroke.points.last()), p);
        case Stroke::Type::Rectangle: {
            if (stroke.points.size() < 2) return kFar;
            const QRectF r = QRectF(scaled(stroke.points.first()), scaled(stroke.points.last())).normalized();
            if (stroke.fillOpacity > 0.0 && r.contains(p)) return 0.0;
            return rectOutlineDistance(r, p);
        }
        case Stroke::Type::Ellipse: {
            if (stroke.points.size() < 2) return kFar;
            const QRectF r = QRectF(scaled(stroke.points.first()), scaled(stroke.points.last())).normalized();
            const double a = std::max(r.width() / 2.0, 1e-6);
            const double b = std::max(r.height() / 2.0, 1e-6);
            const double k = std::hypot((p.x() - r.center().x()) / a, (p.y() - r.center().y()) / b);
            if (stroke.fillOpacity > 0.0 && k <= 1.0) return 0.0;
            return std::fabs(k - 1.0) * std::min(a, b);
        }
        case Stroke::Type::Text: {
            // The real size depends on the picture's pixels, which are not known here: take a box
            // of 4.5% of the height, 2.6% of the height per character, above the anchor.
            const QPointF anchor = scaled(stroke.points.first());
            const double length = std::max<qsizetype>(1, stroke.text.size()) * 0.026;
            const QRectF box(anchor.x(), anchor.y() - 0.045, length, 0.045);
            return box.contains(p) ? 0.0 : rectOutlineDistance(box, p);
        }
    }
    return kFar;
}

int strokeAt(const QVector<Stroke>& strokes, const QPointF& point, double aspect, double tolerance) {
    for (int i = static_cast<int>(strokes.size()) - 1; i >= 0; --i) {  // the last one is on top
        if (distanceToStroke(strokes[i], point, aspect) <= tolerance) return i;
    }
    return -1;
}

Stroke translatedStroke(const Stroke& stroke, const QPointF& delta) {
    Stroke moved = stroke;
    if (moved.points.isEmpty()) return moved;
    double minX = moved.points.first().x();
    double maxX = minX;
    double minY = moved.points.first().y();
    double maxY = minY;
    for (const QPointF& p : moved.points) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    const double dx = std::clamp(delta.x(), -minX, std::max(-minX, 1.0 - maxX));
    const double dy = std::clamp(delta.y(), -minY, std::max(-minY, 1.0 - maxY));
    for (QPointF& p : moved.points) p += QPointF(dx, dy);
    return moved;
}

QVector<QPointF> smoothPoints(const QVector<QPointF>& points, int level) {
    QVector<QPointF> current = points;
    for (int pass = 0; pass < level && current.size() > 2; ++pass) {
        QVector<QPointF> next;
        next.reserve(current.size() * 2);
        next.push_back(current.first());
        for (int i = 0; i + 1 < current.size(); ++i) {
            const QPointF& a = current[i];
            const QPointF& b = current[i + 1];
            next.push_back(a * 0.75 + b * 0.25);
            next.push_back(a * 0.25 + b * 0.75);
        }
        next.push_back(current.last());
        current = next;
    }
    return current;
}

}  // namespace fr
