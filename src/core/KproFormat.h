#pragma once

#include "core/ProjectModel.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

// Project file reader/writer.
//
// PROVISIONAL. The key names were read from string tables of the original application, but
// their nesting and value types are inferred (see README, "Project file"). Everything that
// depends on that guess lives in KproFormat.cpp so it can be corrected in one place once a
// real sample file is available. Do not treat the output as compatible with the original
// application until it has been checked against one.

namespace fr {

inline const QString kProjectDirToken = QStringLiteral("[PROJECT_DIR]");

struct LoadResult {
    bool ok = false;
    QString error;
    QStringList warnings;  // recoverable problems: duplicate ids, unknown parents, ...
    Project project;
};

LoadResult loadProject(const QString& filePath);
LoadResult projectFromJson(const QJsonObject& root);

// `projectDir`, when set, is used to fill each source's relative_path.
QJsonObject projectToJson(const Project& project, const QString& projectDir = {});
bool saveProject(const Project& project, const QString& filePath, QString* error = nullptr);

// Finds the media file for a source: the stored path first, then relative_path against the
// project folder and source_root_dir. Returns the stored path when nothing exists on disk.
QString resolveSourcePath(const Source& source, const Project& project,
                          const QString& projectFilePath);

}  // namespace fr
