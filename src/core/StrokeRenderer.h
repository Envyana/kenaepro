#pragma once

#include "core/ProjectModel.h"

#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QVector>

namespace fr {

// Paints one stroke. `imageRect` is where the whole picture is drawn, in the painter's own
// coordinates; `imageWidth` is the picture's width in its own pixels. Stroke widths are given in
// picture pixels, so they grow and shrink with the picture. `highlighted` adds a dashed box.
void paintStroke(QPainter& painter, const Stroke& stroke, const QRectF& imageRect, double imageWidth,
                 bool highlighted = false);
void paintStrokes(QPainter& painter, const QVector<Stroke>& strokes, const QRectF& imageRect,
                  double imageWidth);

// Distance from `point` to the stroke, in units of the picture's height. Points are normalised to
// the picture; `aspect` is picture width / height, which makes the distance isotropic.
double distanceToStroke(const Stroke& stroke, const QPointF& point, double aspect);
// The topmost stroke within `tolerance` of the point, or -1.
int strokeAt(const QVector<Stroke>& strokes, const QPointF& point, double aspect, double tolerance);
// A copy moved by `delta`, held inside the picture (the delta is shortened rather than the shape
// squeezed).
Stroke translatedStroke(const Stroke& stroke, const QPointF& delta);
// Corner-cutting smoothing of a pencil line. Level 0 returns the points unchanged; each level
// doubles the number of points. The end points stay where they are.
QVector<QPointF> smoothPoints(const QVector<QPointF>& points, int level);

}  // namespace fr
