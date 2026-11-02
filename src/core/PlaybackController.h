#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <functional>

namespace fr {

enum class LoopMode { Off, Loop, PingPong };

struct StepResult {
    int frame = 0;
    int direction = 1;
    bool stopped = false;
};

// Moves `steps` frames from `frame` inside [rangeStart, rangeEnd] (inclusive).
// Pure function so the loop rules can be tested without a timer.
//   Off:      stops on the last frame of the range and reports stopped = true
//   Loop:     wraps to the opposite end of the range
//   PingPong: reverses direction at either end (the end frame is not repeated)
StepResult advanceFrames(int frame, int direction, int steps, int rangeStart, int rangeEnd,
                         LoopMode mode);

// Stepped playback: the frame to show for clock position `position`, given the sorted frames that
// may be shown (`keys`). That is the last key at or before the position inside the range; before the
// first key it is the range's last key (the pose a loop arrives from). -1 when no key is in the range.
int steppedFrame(const QVector<int>& keys, int position, int rangeStart, int rangeEnd);

class PlaybackController : public QObject {
    Q_OBJECT
public:
    explicit PlaybackController(QObject* parent = nullptr);

    void configure(int frameCount, double fps);

    int frameCount() const { return frameCount_; }
    double fps() const { return fps_; }
    int frame() const { return frame_; }
    int rangeStart() const { return rangeStart_; }
    int rangeEnd() const { return rangeEnd_; }
    bool isPlaying() const { return playing_; }
    bool isForward() const { return direction_ > 0; }
    double speed() const { return speed_; }
    LoopMode loopMode() const { return loopMode_; }

    // Stepped playback plays only `keys` (the bookmarked frames): each one is held until the clock
    // reaches the next. Paused stepping and seeking are not affected.
    void setSteppedKeys(const QVector<int>& keys);
    void setStepped(bool stepped);
    bool isStepped() const { return stepped_; }
    // Where the clock is: the frame on screen, or while stepped playback holds a key, the frame it
    // would show without the hold. Sound follows this.
    int clockFrame() const { return holding() ? position_ : frame_; }

    // Asked before each new frame is shown: is it decoded yet? Unless the sound is the clock
    // (setRealtime), playback waits for a frame instead of skipping it, so image sequences play
    // every frame evenly; with sound the picture keeps up with the sound and may skip.
    void setFrameGate(std::function<bool(int)> gate) { gate_ = std::move(gate); }
    void setRealtime(bool realtime) { realtime_ = realtime; }
    bool isRealtime() const { return realtime_; }

public slots:
    void seek(int frame);
    void step(int delta);
    void setRange(int start, int end);
    void resetRange();
    void setSpeed(double speed);
    void setLoopMode(fr::LoopMode mode);
    void play(bool forward = true);
    void pause();
    void togglePlay();

signals:
    void configured();
    void frameChanged(int frame);
    void rangeChanged(int start, int end);
    void playingChanged(bool playing);
    void speedChanged(double speed);
    void looped();  // playback wrapped round the range (the shown frame may not change)

private:
    void onTick();
    void setFrame(int frame);
    bool holding() const;  // stepped playback is on, playing, and has a key to hold

    int frameCount_ = 0;
    double fps_ = 24.0;
    int frame_ = 0;
    int position_ = 0;  // the clock while stepped playback holds a key
    bool stepped_ = false;
    bool realtime_ = false;
    std::function<bool(int)> gate_;
    QVector<int> keys_;
    int rangeStart_ = 0;
    int rangeEnd_ = 0;
    int direction_ = 1;
    double speed_ = 1.0;
    double accumulator_ = 0.0;
    bool playing_ = false;
    LoopMode loopMode_ = LoopMode::Loop;
    QTimer timer_;
    QElapsedTimer clock_;
};

}  // namespace fr
