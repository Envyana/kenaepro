#include "core/ProjectModel.h"

#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <iterator>
#include <utility>

namespace fr {

QString strokeTypeName(Stroke::Type type) {
    switch (type) {
        case Stroke::Type::Pencil: return QStringLiteral("pencil");
        case Stroke::Type::Arrow: return QStringLiteral("arrow");
        case Stroke::Type::Ellipse: return QStringLiteral("ellipse");
        case Stroke::Type::Rectangle: return QStringLiteral("rectangle");
        case Stroke::Type::Text: return QStringLiteral("text");
    }
    return QStringLiteral("pencil");
}

std::optional<Stroke::Type> strokeTypeFromName(const QString& name) {
    const QString n = name.toLower();
    if (n == QLatin1String("pencil")) return Stroke::Type::Pencil;
    if (n == QLatin1String("arrow")) return Stroke::Type::Arrow;
    if (n == QLatin1String("ellipse")) return Stroke::Type::Ellipse;
    if (n == QLatin1String("rectangle")) return Stroke::Type::Rectangle;
    if (n == QLatin1String("text")) return Stroke::Type::Text;
    return std::nullopt;
}

std::optional<int> adjacentFrame(const QVector<int>& sorted, int frame, int direction) {
    if (sorted.isEmpty()) return std::nullopt;
    if (direction >= 0) {
        const auto next = std::upper_bound(sorted.begin(), sorted.end(), frame);
        return next != sorted.end() ? *next : sorted.first();
    }
    const auto at = std::lower_bound(sorted.begin(), sorted.end(), frame);
    return at != sorted.begin() ? *std::prev(at) : sorted.last();
}

QVector<Stroke> Source::visibleStrokes(int frame) const {
    const FrameAnnotation* own = annotationAt(frame);
    if (own && !own->strokes.isEmpty()) return own->strokes;
    if (!holdAnnotations) return {};
    const FrameAnnotation* held = nullptr;
    for (const FrameAnnotation& a : annotations) {
        if (a.frame < frame && !a.strokes.isEmpty() && (!held || a.frame > held->frame)) held = &a;
    }
    return held ? held->strokes : QVector<Stroke>();
}

std::pair<int, int> Source::cutRange(int frameCount) const {
    const int last = std::max(0, frameCount - 1);
    const int in = std::clamp(cutInFrame, 0, last);
    const int out = cutOutFrame < 0 ? last : std::clamp(cutOutFrame, in, last);
    return {in, out};
}

const FrameAnnotation* Source::annotationAt(int frame) const {
    for (const FrameAnnotation& a : annotations) {
        if (a.frame == frame) return &a;
    }
    return nullptr;
}

FrameAnnotation& Source::annotationForEdit(int frame) {
    for (FrameAnnotation& a : annotations) {
        if (a.frame == frame) return a;
    }
    FrameAnnotation fresh;
    fresh.frame = frame;
    annotations.push_back(fresh);
    return annotations.back();
}

void Source::pruneEmptyAnnotations() {
    annotations.erase(std::remove_if(annotations.begin(), annotations.end(),
                                     [](const FrameAnnotation& a) { return a.isEmpty(); }),
                      annotations.end());
}

void Source::normalizeBookmarks() {
    frameBookmarks.erase(std::remove_if(frameBookmarks.begin(), frameBookmarks.end(),
                                        [](int frame) { return frame < 0; }),
                         frameBookmarks.end());
    std::sort(frameBookmarks.begin(), frameBookmarks.end());
    frameBookmarks.erase(std::unique(frameBookmarks.begin(), frameBookmarks.end()), frameBookmarks.end());

    rangeBookmarks.erase(std::remove_if(rangeBookmarks.begin(), rangeBookmarks.end(),
                                        [](const RangeBookmark& r) { return r.start < 0 || r.end < r.start; }),
                         rangeBookmarks.end());
    std::sort(rangeBookmarks.begin(), rangeBookmarks.end(), [](const RangeBookmark& a, const RangeBookmark& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    rangeBookmarks.erase(std::unique(rangeBookmarks.begin(), rangeBookmarks.end()), rangeBookmarks.end());
}

QVector<int> Source::bookmarkTargets() const {
    QVector<int> targets = frameBookmarks;
    for (const RangeBookmark& range : rangeBookmarks) targets.push_back(range.start);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

std::optional<int> Source::adjacentBookmark(int frame, int direction) const {
    return adjacentFrame(bookmarkTargets(), frame, direction);
}

Project Project::createEmpty() {
    Project p;
    p.ensureBaseFolders();
    return p;
}

bool Project::isBaseFolder(const QString& id) {
    return id == kSourcesFolderId || id == kTimelinesFolderId;
}

void Project::ensureBaseFolders() {
    if (!findFolder(kSourcesFolderId)) {
        folders.prepend(Folder{kSourcesFolderId, QStringLiteral("SOURCES"), {}, true});
    }
    if (!findFolder(kTimelinesFolderId)) {
        const int at = std::min<int>(1, folders.size());
        folders.insert(at, Folder{kTimelinesFolderId, QStringLiteral("TIMELINES"), {}, true});
    }
}

const Folder* Project::findFolder(const QString& id) const {
    for (const Folder& f : folders) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

Folder* Project::findFolder(const QString& id) {
    return const_cast<Folder*>(std::as_const(*this).findFolder(id));
}

const Source* Project::findSource(const QString& id) const {
    for (const Source& s : sources) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

Source* Project::findSource(const QString& id) {
    return const_cast<Source*>(std::as_const(*this).findSource(id));
}

const Timeline* Project::findTimeline(const QString& id) const {
    for (const Timeline& t : timelines) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

Timeline* Project::findTimeline(const QString& id) {
    return const_cast<Timeline*>(std::as_const(*this).findTimeline(id));
}

bool Project::idInUse(const QString& id) const {
    return findFolder(id) || findSource(id) || findTimeline(id);
}

QString Project::newId() const {
    QString id;
    do {
        id = QUuid::createUuid().toString(QUuid::Id128).left(8);
    } while (idInUse(id));
    return id;
}

QString Project::addFolder(const QString& name, const QString& parentId) {
    const QString parent = parentId.isEmpty() ? kSourcesFolderId : parentId;
    if (!findFolder(parent)) return {};
    Folder f;
    f.id = newId();
    f.name = name.isEmpty() ? QStringLiteral("New Folder") : name;
    f.parentId = parent;
    folders.push_back(f);
    return f.id;
}

QString Project::addSource(const QString& path, const QString& name, const QString& parentId) {
    const QString parent = parentId.isEmpty() ? kSourcesFolderId : parentId;
    if (!findFolder(parent)) return {};
    Source s;
    s.id = newId();
    s.name = name.isEmpty() ? QFileInfo(path).fileName() : name;
    s.path = path;
    s.parentId = parent;
    sources.push_back(s);
    return s.id;
}

QString Project::addTimeline(const QString& name, const QString& parentId) {
    const QString parent = parentId.isEmpty() ? kTimelinesFolderId : parentId;
    if (!findFolder(parent)) return {};
    Timeline t;
    t.id = newId();
    t.name = name.isEmpty() ? QStringLiteral("New Timeline") : name;
    t.parentId = parent;
    t.isDefault = (defaultTimeline() == nullptr);
    timelines.push_back(t);
    return t.id;
}

RemoveStatus Project::removeFolder(const QString& id) {
    if (!findFolder(id)) return RemoveStatus::NotFound;
    if (isBaseFolder(id)) return RemoveStatus::BaseFolder;
    const auto childOf = [&id](const auto& item) { return item.parentId == id; };
    if (std::any_of(folders.begin(), folders.end(), childOf) ||
        std::any_of(sources.begin(), sources.end(), childOf) ||
        std::any_of(timelines.begin(), timelines.end(), childOf)) {
        return RemoveStatus::NotEmpty;
    }
    folders.erase(std::remove_if(folders.begin(), folders.end(),
                                 [&id](const Folder& f) { return f.id == id; }),
                  folders.end());
    return RemoveStatus::Ok;
}

RemoveStatus Project::removeSource(const QString& id, bool force) {
    if (!findSource(id)) return RemoveStatus::NotFound;
    const auto uses = [&id](const Timeline& t) {
        return std::any_of(t.clips.begin(), t.clips.end(),
                           [&id](const Clip& c) { return c.sourceId == id; });
    };
    if (std::any_of(timelines.begin(), timelines.end(), uses)) {
        if (!force) return RemoveStatus::SourceInTimelines;
        for (Timeline& t : timelines) {
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                         [&id](const Clip& c) { return c.sourceId == id; }),
                          t.clips.end());
        }
    }
    sources.erase(std::remove_if(sources.begin(), sources.end(),
                                 [&id](const Source& s) { return s.id == id; }),
                  sources.end());
    return RemoveStatus::Ok;
}

RemoveStatus Project::removeTimeline(const QString& id) {
    Timeline* t = findTimeline(id);
    if (!t) return RemoveStatus::NotFound;
    const bool wasDefault = t->isDefault;
    timelines.erase(std::remove_if(timelines.begin(), timelines.end(),
                                   [&id](const Timeline& x) { return x.id == id; }),
                    timelines.end());
    if (wasDefault && !timelines.isEmpty()) timelines.first().isDefault = true;
    return RemoveStatus::Ok;
}

const Timeline* Project::defaultTimeline() const {
    for (const Timeline& t : timelines) {
        if (t.isDefault) return &t;
    }
    return nullptr;
}

bool Project::setDefaultTimeline(const QString& id) {
    if (!findTimeline(id)) return false;
    for (Timeline& t : timelines) t.isDefault = (t.id == id);
    return true;
}

}  // namespace fr
