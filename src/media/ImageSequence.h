#pragma once

#include "media/FrameSource.h"

#include <QSize>
#include <QString>
#include <QVector>

#include <optional>

namespace fr {

// A numbered run of image files such as shot.0001.png ... shot.0120.png.
struct SequencePattern {
    QString directory;  // may be empty for a relative pattern
    QString prefix;     // "shot."
    QString suffix;     // ".png"
    int digits = 0;     // zero-padded width, or 0 when the numbers are not padded

    QString pathFor(int number) const;
    // The printf-style form kept in a project as the source path: "/dir/shot.%04d.png".
    QString pattern() const;
};

// True for a path written in the printf form ("%04d" or "%d" right before the extension).
bool isSequencePath(const QString& path);
std::optional<SequencePattern> parseSequencePath(const QString& path);
// Works out the pattern a numbered file belongs to (the last group of digits in its name).
std::optional<SequencePattern> sequenceFromFile(const QString& filePath);
// The frame numbers that exist on disk, ascending.
QVector<int> scanSequence(const SequencePattern& pattern);
// True when Qt has a reader plugin for the file's extension.
bool isImageFile(const QString& path);

struct SequenceGroup {
    SequencePattern pattern;
    QVector<int> numbers;
};
// Every run of at least two numbered images in a folder.
QVector<SequenceGroup> findSequencesInFolder(const QString& folder);

// The frames of an image sequence, decoded with Qt's image readers. Gaps in the numbering are
// skipped, so frame index i is the i-th file that exists, not number i.
class ImageSequenceSource final : public FrameSource {
public:
    // `path` is the printf form of the pattern.
    bool open(const QString& path, QString* error) override;
    int frameCount() const override { return static_cast<int>(numbers_.size()); }
    double fps() const override { return 0.0; }  // image sequences have no rate of their own
    QSize size() const override { return size_; }
    QImage frame(int index) override;
    bool concurrentFrames() const override { return true; }  // one file per frame, nothing shared
    QVector<int> frameNumbers() const override { return numbers_; }

private:
    SequencePattern pattern_;
    QVector<int> numbers_;
    QSize size_;
};

}  // namespace fr
