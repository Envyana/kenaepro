#pragma once

#include <QString>

namespace fr {

// How the frame counter is written.
enum class NumberingMode {
    Global,    // 1-based position in what is shown, plus the time-control offset
    Image,     // the number in the image file name (image sequences); falls back to Global
    Range,     // 1-based position inside the playback range
    Timecode,  // HH:MM:SS:FF
};

struct FrameLabelInput {
    int frame = 0;        // 0-based, in what is shown
    int frameCount = 0;
    int rangeStart = 0;   // playback range, 0-based and inclusive
    int rangeEnd = 0;
    int tcOffset = 0;     // frames added to the global number and to the timecode
    double fps = 24.0;
    int imageNumber = -1; // number in the file name, or -1
};

// "HH:MM:SS:FF" for a frame count at a frame rate. The rate is rounded to a whole number and
// drop-frame timecode is not used.
QString formatTimecode(int frames, double fps);

// Clock time of a frame count at a frame rate, for people: "MM:SS.mmm", or "H:MM:SS.mmm" from one
// hour up. Unlike the timecode the exact rate is used, so 24 frames at 23.976 are 00:01.001.
QString formatDuration(int frames, double fps);

QString formatFrameLabel(NumberingMode mode, const FrameLabelInput& input);

}  // namespace fr
