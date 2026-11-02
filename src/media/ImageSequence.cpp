#include "media/ImageSequence.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace fr {
namespace {

// "%04d" or "%d", only when it sits right before the extension, so a name like "100%done.mp4"
// is not mistaken for a pattern.
const QRegularExpression& placeholderExpression() {
    static const QRegularExpression expression(QStringLiteral("%(0(\\d+))?d(?=\\.[^./]*$)"));
    return expression;
}

bool allDigits(const QString& text) {
    if (text.isEmpty()) return false;
    for (const QChar c : text) {
        if (!c.isDigit()) return false;
    }
    return true;
}

QString joinDirectory(const QString& directory, const QString& name) {
    return directory.isEmpty() ? name : directory + QLatin1Char('/') + name;
}

}  // namespace

QString SequencePattern::pathFor(int number) const {
    QString text = QString::number(number);
    if (digits > 0) text = text.rightJustified(digits, QLatin1Char('0'));
    return joinDirectory(directory, prefix + text + suffix);
}

QString SequencePattern::pattern() const {
    const QString placeholder = digits > 0
                                    ? QStringLiteral("%0") + QString::number(digits) + QLatin1Char('d')
                                    : QStringLiteral("%d");
    return joinDirectory(directory, prefix + placeholder + suffix);
}

bool isSequencePath(const QString& path) {
    return placeholderExpression().match(path).hasMatch();
}

std::optional<SequencePattern> parseSequencePath(const QString& path) {
    const QRegularExpressionMatch match = placeholderExpression().match(path);
    if (!match.hasMatch()) return std::nullopt;

    SequencePattern result;
    result.digits = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
    result.suffix = path.mid(match.capturedEnd());
    const QString before = path.left(match.capturedStart());
    const int slash = before.lastIndexOf(QLatin1Char('/'));
    result.directory = slash >= 0 ? before.left(slash) : QString();
    result.prefix = before.mid(slash + 1);
    return result;
}

std::optional<SequencePattern> sequenceFromFile(const QString& filePath) {
    const QFileInfo info(filePath);
    const QString base = info.completeBaseName();
    qsizetype start = base.size();
    while (start > 0 && base.at(start - 1).isDigit()) --start;
    if (start == base.size()) return std::nullopt;  // the name does not end in a number

    const QString digits = base.mid(start);
    SequencePattern padded;
    padded.directory = info.absolutePath();
    padded.prefix = base.left(start);
    padded.suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    padded.digits = static_cast<int>(digits.size());
    SequencePattern plain = padded;
    plain.digits = 0;

    // "frame12.png" may belong to an unpadded run (frame9, frame10, ...) or to a padded one whose
    // first digits happen to be non-zero (frame1000 ... frame1200). Count the files each reading
    // finds and keep the larger run; on a tie prefer the padded one when there is a leading zero.
    const qsizetype paddedCount = scanSequence(padded).size();
    const qsizetype plainCount = scanSequence(plain).size();
    if (paddedCount != plainCount) return paddedCount > plainCount ? padded : plain;
    return (digits.size() > 1 && digits.startsWith(QLatin1Char('0'))) ? padded : plain;
}

QVector<int> scanSequence(const SequencePattern& pattern) {
    QVector<int> numbers;
    const QDir directory(pattern.directory.isEmpty() ? QStringLiteral(".") : pattern.directory);
    const QStringList names = directory.entryList(QDir::Files);
    for (const QString& name : names) {
        if (!name.startsWith(pattern.prefix) || !name.endsWith(pattern.suffix)) continue;
        if (name.size() <= pattern.prefix.size() + pattern.suffix.size()) continue;
        const QString middle =
            name.mid(pattern.prefix.size(), name.size() - pattern.prefix.size() - pattern.suffix.size());
        if (!allDigits(middle)) continue;
        if (pattern.digits > 0) {
            if (middle.size() != pattern.digits) continue;
        } else if (middle.size() > 1 && middle.startsWith(QLatin1Char('0'))) {
            continue;  // zero-padded names belong to a padded pattern
        }
        bool ok = false;
        const int value = middle.toInt(&ok);
        if (ok) numbers.push_back(value);
    }
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    return numbers;
}

bool isImageFile(const QString& path) {
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        for (const QByteArray& format : QImageReader::supportedImageFormats()) {
            set.insert(QString::fromLatin1(format).toLower());
        }
        return set;
    }();
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

QVector<SequenceGroup> findSequencesInFolder(const QString& folder) {
    QVector<SequenceGroup> groups;
    QSet<QString> seen;
    const QDir directory(folder);
    QSet<QString> covered;  // files of the groups found so far: they need no scan of their own
    for (const QString& name : directory.entryList(QDir::Files, QDir::Name)) {
        const QString path = directory.absoluteFilePath(name);
        if (covered.contains(path) || !isImageFile(path)) continue;
        const std::optional<SequencePattern> pattern = sequenceFromFile(path);
        if (!pattern) continue;
        const QString key = pattern->pattern();
        if (seen.contains(key)) continue;
        seen.insert(key);
        const QVector<int> numbers = scanSequence(*pattern);
        if (numbers.size() >= 2) {
            for (const int number : numbers) covered.insert(QFileInfo(pattern->pathFor(number)).absoluteFilePath());
            groups.push_back(SequenceGroup{*pattern, numbers});
        }
    }
    return groups;
}

bool ImageSequenceSource::open(const QString& path, QString* error) {
    const std::optional<SequencePattern> parsed = parseSequencePath(path);
    if (!parsed) {
        if (error) *error = QStringLiteral("Not an image sequence pattern: %1").arg(path);
        return false;
    }
    pattern_ = *parsed;
    numbers_ = scanSequence(pattern_);
    if (numbers_.isEmpty()) {
        if (error) *error = QStringLiteral("No files match %1").arg(path);
        return false;
    }

    QImageReader reader(pattern_.pathFor(numbers_.first()));
    size_ = reader.size();
    if (!size_.isValid()) {
        const QImage first = reader.read();
        if (first.isNull()) {
            if (error) *error = reader.errorString();
            return false;
        }
        size_ = first.size();
    }
    return true;
}

QImage ImageSequenceSource::frame(int index) {
    if (index < 0 || index >= numbers_.size()) return {};
    QImageReader reader(pattern_.pathFor(numbers_[index]));
    reader.setAutoTransform(true);
    return reader.read();
}

}  // namespace fr
