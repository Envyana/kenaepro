#include "core/PlaybackController.h"

#include <QtGlobal>

#include <algorithm>

namespace fr {

StepResult advanceFrames(int frame, int direction, int steps, int rangeStart, int rangeEnd,
                         LoopMode mode) {
    StepResult r;
    r.frame = frame;
    r.direction = direction >= 0 ? 1 : -1;
    if (rangeEnd < rangeStart) {
        r.stopped = true;
        return r;
    }
    for (int i = 0; i < steps; ++i) {
        int next = r.frame + r.direction;
        if (next > rangeEnd || next < rangeStart) {
            switch (mode) {
                case LoopMode::Off:
                    r.stopped = true;
                    return r;
                case LoopMode::Loop:
                    next = r.direction > 0 ? rangeStart : rangeEnd;
                    break;
                case LoopMode::PingPong:
                    r.direction = -r.direction;
                    next = r.frame + r.direction;
                    if (next > rangeEnd || next < rangeStart) {  // single-frame range
                        r.stopped = true;
                        return r;
                    }
                    break;
            }
        }
        r.frame = next;
    }
    return r;
}

int steppedFrame(const QVector<int>& keys, int position, int rangeStart, int rangeEnd) {
    int before = -1;
    int last = -1;
    for (const int key : keys) {
        if (key < rangeStart || key > rangeEnd) continue;
        if (key <= position) before = key;
        last = key;
    }
    return before >= 0 ? before : last;
}

PlaybackController::PlaybackController(QObject* parent) : QObject(parent) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(4);
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::onTick);
}

void PlaybackController::configure(int frameCount, double fps) {
    pause();
    frameCount_ = std::max(0, frameCount);
    fps_ = fps > 0.0 ? fps : 24.0;
    rangeStart_ = 0;
    rangeEnd_ = std::max(0, frameCount_ - 1);
    frame_ = 0;
    direction_ = 1;
    emit configured();
    emit rangeChanged(rangeStart_, rangeEnd_);
    emit frameChanged(frame_);
}

void PlaybackController::setFrame(int frame) {
    if (frame == frame_) return;
    frame_ = frame;
    emit frameChanged(frame_);
}

void PlaybackController::seek(int frame) {
    if (frameCount_ <= 0) return;
    frame = std::clamp(frame, 0, frameCount_ - 1);
    position_ = frame;
    if (holding()) frame = steppedFrame(keys_, position_, rangeStart_, rangeEnd_);
    setFrame(frame);
}

void PlaybackController::setSteppedKeys(const QVector<int>& keys) {
    keys_ = keys;
    std::sort(keys_.begin(), keys_.end());
}

void PlaybackController::setStepped(bool stepped) {
    if (stepped == stepped_) return;
    stepped_ = stepped;
    position_ = frame_;
}

bool PlaybackController::holding() const {
    return stepped_ && playing_ && steppedFrame(keys_, position_, rangeStart_, rangeEnd_) >= 0;
}

void PlaybackController::step(int delta) {
    pause();
    seek(frame_ + delta);
}

void PlaybackController::setRange(int start, int end) {
    if (frameCount_ <= 0) return;
    start = std::clamp(start, 0, frameCount_ - 1);
    end = std::clamp(end, 0, frameCount_ - 1);
    if (end < start) return;
    rangeStart_ = start;
    rangeEnd_ = end;
    emit rangeChanged(rangeStart_, rangeEnd_);
}

void PlaybackController::resetRange() {
    setRange(0, frameCount_ - 1);
}

void PlaybackController::setSpeed(double speed) {
    speed = std::clamp(speed, 0.05, 8.0);
    if (speed == speed_) return;
    speed_ = speed;
    emit speedChanged(speed_);
}

void PlaybackController::setLoopMode(LoopMode mode) {
    loopMode_ = mode;
}

void PlaybackController::play(bool forward) {
    if (frameCount_ <= 0) return;
    direction_ = forward ? 1 : -1;
    const int edge = forward ? rangeEnd_ : rangeStart_;
    // Stepped playback that ran to the end stopped on a held key; the clock is what reached it.
    const int at = stepped_ ? position_ : frame_;
    if (at < rangeStart_ || at > rangeEnd_ || (loopMode_ == LoopMode::Off && at == edge)) {
        setFrame(forward ? rangeStart_ : rangeEnd_);
        position_ = frame_;
    } else if (!stepped_ || steppedFrame(keys_, position_, rangeStart_, rangeEnd_) != frame_) {
        position_ = frame_;  // moved since (a seek, a step): start from what is shown
    }
    accumulator_ = 0.0;
    clock_.start();
    timer_.start();
    if (!playing_) {
        playing_ = true;
        emit playingChanged(true);
    }
}

void PlaybackController::pause() {
    timer_.stop();
    if (playing_) {
        playing_ = false;
        emit playingChanged(false);
    }
}

void PlaybackController::togglePlay() {
    if (playing_) {
        pause();
    } else {
        play(direction_ > 0);
    }
}

void PlaybackController::onTick() {
    const double elapsed = static_cast<double>(clock_.nsecsElapsed()) * 1e-9;
    clock_.restart();
    accumulator_ += elapsed * fps_ * speed_;
    int steps = static_cast<int>(accumulator_);
    if (steps <= 0) return;

    const bool hold = holding();
    int from = hold ? position_ : frame_;
    // The range was moved past the playhead while playing: carry on from its start.
    if (from < rangeStart_ || from > rangeEnd_) from = direction_ > 0 ? rangeStart_ : rangeEnd_;
    const auto shown = [&](int clock) { return hold ? steppedFrame(keys_, clock, rangeStart_, rangeEnd_) : clock; };

    StepResult r;
    if (gate_ && !realtime_) {
        // One frame at a time, and only once it is decoded; a backlog is not caught up in a rush.
        r = advanceFrames(from, direction_, 1, rangeStart_, rangeEnd_, loopMode_);
        if (!r.stopped && !gate_(shown(r.frame))) {
            accumulator_ = std::min(accumulator_, 1.0);
            return;
        }
        accumulator_ = std::min(accumulator_ - 1.0, 1.0);
    } else {
        accumulator_ -= steps;
        steps = std::min(steps, std::max(1, frameCount_));  // after a stall, don't spin through many laps
        r = advanceFrames(from, direction_, steps, rangeStart_, rangeEnd_, loopMode_);
    }
    const bool wrapped = loopMode_ == LoopMode::Loop && (r.frame - from) * direction_ < 0;
    direction_ = r.direction;
    position_ = r.frame;
    setFrame(shown(r.frame));
    if (wrapped) emit looped();
    if (r.stopped) pause();
}

}  // namespace fr
