cmake_minimum_required(VERSION 3.21)
project(KenaePRO VERSION 0.3.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Network Test)

# FFmpeg is optional so the app (and the tests) build on a machine without it.
# Without FFmpeg only still images can be opened.
option(FR_WITH_FFMPEG "Enable FFmpeg video decoding" ON)
set(FR_FFMPEG_FOUND OFF)
if(FR_WITH_FFMPEG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
        if(FFMPEG_FOUND)
            set(FR_FFMPEG_FOUND ON)
        endif()
    endif()
    if(NOT FR_FFMPEG_FOUND)
        message(WARNING "FFmpeg not found (needs pkg-config + libav*-dev): building without video support")
    endif()
endif()

add_library(framereview_core STATIC
    src/core/ProjectModel.cpp
    src/core/ProjectModel.h
    src/core/AnnotationCommands.cpp
    src/core/AnnotationCommands.h
    src/core/KproFormat.cpp
    src/core/KproFormat.h
    src/core/PlaybackController.cpp
    src/core/PlaybackController.h
    src/core/Session.cpp
    src/core/Session.h
    src/media/FrameSource.cpp
    src/media/FrameSource.h
    src/media/AsyncFrameSource.cpp
    src/media/AsyncFrameSource.h
    src/media/ImageSequence.cpp
    src/media/ImageSequence.h
    src/media/ClipSequenceSource.cpp
    src/media/ClipSequenceSource.h
    src/media/AudioTrack.cpp
    src/media/AudioTrack.h
    src/media/AudioMix.cpp
    src/media/AudioMix.h
    src/core/FrameNumbering.cpp
    src/core/FrameNumbering.h
    src/core/StrokeRenderer.cpp
    src/core/StrokeRenderer.h
    src/core/Exporter.cpp
    src/core/Exporter.h
    src/core/Preferences.cpp
    src/core/Preferences.h
    src/core/AudioController.cpp
    src/core/AudioController.h
    src/api/ApiDispatcher.cpp
    src/api/ApiDispatcher.h
    src/api/ApiServer.cpp
    src/api/ApiServer.h
)
target_include_directories(framereview_core PUBLIC src)
target_link_libraries(framereview_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network)

if(FR_FFMPEG_FOUND)
    target_sources(framereview_core PRIVATE
        src/media/FfmpegFrameSource.cpp
        src/media/FfmpegFrameSource.h
    )
    target_compile_definitions(framereview_core PUBLIC FR_HAVE_FFMPEG=1)
    target_link_libraries(framereview_core PUBLIC PkgConfig::FFMPEG)
endif()

# Sound output needs Qt Multimedia; without it the app is silent (everything else still works).
option(FR_WITH_AUDIO "Enable sound playback through Qt Multimedia" ON)
if(FR_WITH_AUDIO AND FR_FFMPEG_FOUND)
    find_package(Qt6 6.5 QUIET COMPONENTS Multimedia)
    if(Qt6Multimedia_FOUND)
        target_compile_definitions(framereview_core PUBLIC FR_HAVE_AUDIO=1)
        target_link_libraries(framereview_core PUBLIC Qt6::Multimedia)
    else()
        message(WARNING "Qt6 Multimedia not found: building without sound")
    endif()
endif()

# The widgets live in a library so the UI tests can link them.
add_library(framereview_ui STATIC
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ViewerWidget.cpp
    src/ui/ViewerWidget.h
    src/ui/BookmarkSlider.cpp
    src/ui/BookmarkSlider.h
    src/ui/ViewerArea.cpp
    src/ui/ViewerArea.h
    src/ui/TimelinePanel.cpp
    src/ui/TimelinePanel.h
    src/ui/ExportDialog.cpp
    src/ui/ExportDialog.h
    src/ui/PreferencesDialog.cpp
    src/ui/PreferencesDialog.h
)
target_link_libraries(framereview_ui PUBLIC framereview_core Qt6::Widgets Qt6::OpenGLWidgets)

add_executable(kenaepro src/main.cpp)
target_compile_definitions(kenaepro PRIVATE FR_VERSION="${PROJECT_VERSION}")
target_link_libraries(kenaepro PRIVATE framereview_ui)

enable_testing()
add_subdirectory(tests)

include(GNUInstallDirs)
install(TARGETS kenaepro RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/kenaepro.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/kenaepro.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
