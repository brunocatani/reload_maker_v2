QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = LibraryTests
isEmpty(NIFSKOPE_SOURCE_DIR): error(NIFSKOPE_SOURCE_DIR is required)
LIBFO = $$NIFSKOPE_SOURCE_DIR/lib/libfo76utils/src
INCLUDEPATH += ../src $$LIBFO
SOURCES += LibraryTests.cpp ../src/LoadOrderLibrary.cpp ../src/LibraryResources.cpp ../src/PackageSource.cpp ../src/EspReader.cpp ../src/WeaponConfiguration.cpp \
    $$LIBFO/ba2file.cpp $$LIBFO/common.cpp $$LIBFO/filebuf.cpp $$LIBFO/zlib.cpp

SOURCES += ../src/StudioPaths.cpp ../src/areas/sources/SourceBrowserRows.cpp
