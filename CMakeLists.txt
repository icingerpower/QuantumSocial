cmake_minimum_required(VERSION 3.5)

project(QuantumSocial VERSION 0.1 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

# Compile and install QCoro (used for the coroutine-based statistics fetch):
#   git clone https://github.com/qcoro/qcoro.git && cd qcoro && mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/home/cedric/Qt/6.8.3/gcc_64 -DUSE_QT_VERSION=6 \
#            -DCMAKE_BUILD_TYPE=Release
#   make && sudo make install
# setup_qcoro() (mirrors ../AmazonTemplate3/common.cmake) tries several install
# layouts since the QCoro6 config file location varies by distro/build.
include(common.cmake)
setup_qcoro(COMPONENTS Core)

include(../common/aicli/aicli.cmake)
include(../common/gui/progress/progress.cmake)
include(../common/workingdirectory/workingdirectory.cmake)
include(model/model.cmake)
include(gui/gui.cmake)

set(PROJECT_SOURCES
        main.cpp
        ${AICLI_FILES}
        ${PROGRESS_DIALOG_FILES}
        ${GUI_FILES}
        ${MODEL_FILES}
        ${WORKING_DIR_FILES}
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(QuantumSocial
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
else()
    if(ANDROID)
        add_library(QuantumSocial SHARED
            ${PROJECT_SOURCES}
        )
    else()
        add_executable(QuantumSocial
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(QuantumSocial PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Multimedia
    Qt${QT_VERSION_MAJOR}::MultimediaWidgets
    QCoro::Core)
target_include_directories(QuantumSocial PRIVATE ../common/aicli ../common/gui/progress)

# Absolute path to the statistics-fetching Python worker, so the app can
# launch it via QProcess with zero configuration in a source build.
target_compile_definitions(QuantumSocial PRIVATE
    SOCIAL_STATS_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/model/accounts"
    VIDEO_GEN_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/model/videogen")

set_target_properties(QuantumSocial PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER my.example.com
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

install(TARGETS QuantumSocial
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(QuantumSocial)
endif()
