# PenCast for Linux (Qt 6). Build:  cmake -B build -S linux && cmake --build build && ./build/pencast deck.pdf
cmake_minimum_required(VERSION 3.21)
project(PenCast VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Pdf PdfWidgets Multimedia Test)

# Logic without windows or PDF rendering, shared with the test.
add_library(pencastcore STATIC src/core.cpp src/pdf.cpp src/media.cpp)
target_link_libraries(pencastcore PUBLIC Qt6::Gui)
target_include_directories(pencastcore PUBLIC src)

add_executable(pencast src/main.cpp src/deck.cpp src/slideview.cpp src/playback.cpp src/playback.h
               src/tooloptions.cpp src/tooloptions.h)
target_link_libraries(pencast PRIVATE pencastcore Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets Qt6::Multimedia)
target_compile_definitions(pencast PRIVATE PENCAST_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS pencast)
install(FILES pencast.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES pencast.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)

# Packages: `cpack -G DEB` or `cpack -G RPM` after a Release build (see .github/workflows/linux.yml).
set(CPACK_PACKAGE_NAME pencast)
set(CPACK_PACKAGE_VENDOR "Gregory Janes")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Present LaTeX Beamer PDFs with a laser pointer and pen")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/e1ind/pencast")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Gregory Janes <https://github.com/e1ind/pencast>")
set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)   # depends on the distribution's Qt packages
# Video: Qt's GStreamer backend crashes without the base and good plugins; libav decodes H.264.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-libav")
set(CPACK_RPM_PACKAGE_REQUIRES "gstreamer1-plugins-base, gstreamer1-plugins-good")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE GPL-3.0-or-later)
set(CPACK_RPM_PACKAGE_GROUP Applications/Productivity)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share/applications /usr/share/icons /usr/share/icons/hicolor
    /usr/share/icons/hicolor/256x256 /usr/share/icons/hicolor/256x256/apps)
include(CPack)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE pencastcore)
add_test(NAME core COMMAND core_test)

add_executable(input_test tests/input_test.cpp src/deck.cpp src/slideview.cpp src/playback.cpp src/playback.h)
target_link_libraries(input_test PRIVATE pencastcore Qt6::Widgets Qt6::Pdf Qt6::Multimedia Qt6::Test)
target_compile_definitions(input_test PRIVATE DECK_PDF="${CMAKE_CURRENT_SOURCE_DIR}/../Corpus/stress_100.pdf"
                           CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/../Corpus")
add_test(NAME input COMMAND input_test)

add_executable(media_test tests/media_test.cpp)
target_link_libraries(media_test PRIVATE pencastcore Qt6::Pdf)
target_compile_definitions(media_test PRIVATE CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/../Corpus")
add_test(NAME media COMMAND media_test)
