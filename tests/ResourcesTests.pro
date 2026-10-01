QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = ResourcesTests
isEmpty(NIFSKOPE_SOURCE_DIR): error(NIFSKOPE_SOURCE_DIR is required)
LIBFO = $$NIFSKOPE_SOURCE_DIR/lib/libfo76utils/src
INCLUDEPATH += ../src $$LIBFO
SOURCES += ResourcesTests.cpp ../src/PackageSource.cpp \
    $$LIBFO/ba2file.cpp $$LIBFO/common.cpp $$LIBFO/filebuf.cpp $$LIBFO/zlib.cpp
HEADERS += ../src/PackageSource.h

SOURCES += ../src/StudioPaths.cpp ../src/SourceEvidence.cpp
