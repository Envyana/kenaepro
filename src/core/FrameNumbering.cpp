#include "core/FrameNumbering.h"

#include <algorithm>
#include <cmath>

namespace fr {

QString formatTimecode(int frames, double fps) {
    const int rate = std::max(1, static_cast<int>(std::lround(fps)));
    const int total = std::max(0, frames);
    const int ff = total % rate;
    const int seconds = total / rate;
    const auto two = [](int value) { return QString::number(value).rightJustified(2, QLatin1Char('0')); };
    return two(seconds / 3600) + QLatin1Char(':') + two((seconds / 60) % 60) + QLatin1Char(':') +
           two(seconds % 60) + QLatin1Char(':') + two(ff);
}

QString formatDuration(int frames, double fps) {
    const double rate = fps > 0.0 ? fps : 24.0;
    const qint64 ms = std::llround(std::max(0, frames) * 1000.0 / rate);
    const qint64 seconds = ms / 1000;
    const auto pad = [](qint64 value, int width) {
        return QString::number(value).rightJustified(width, QLatin1Char('0'));
    };
    QString text = pad((seconds / 60) % 60, 2) + QLatin1Char(':') + pad(seconds % 60, 2) + QLatin1Char('.') +
                   pad(ms % 1000, 3);
    if (seconds >= 3600) text.prepend(QString::number(seconds / 3600) + QLatin1Char(':'));
    return text;
}

QString formatFrameLabel(NumberingMode mode, const FrameLabelInput& in) {
    if (in.frameCount <= 0) return {};
    switch (mode) {
        case NumberingMode::Image:
            if (in.imageNumber >= 0) return QStringLiteral("#%1").arg(in.imageNumber);
            [[fallthrough]];
        case NumberingMode::Global:
            return QStringLiteral("%1 / %2").arg(in.frame + 1 + in.tcOffset).arg(in.frameCount + in.tcOffset);
        case NumberingMode::Range: {
            const int length = std::max(1, in.rangeEnd - in.rangeStart + 1);
            return QStringLiteral("%1 / %2").arg(in.frame - in.rangeStart + 1).arg(length);
        }
        case NumberingMode::Timecode:
            return formatTimecode(in.frame + in.tcOffset, in.fps);
    }
    return {};
}

}  // namespace fr
