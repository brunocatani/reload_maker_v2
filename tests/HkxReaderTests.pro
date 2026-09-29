QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = HkxReaderTests
INCLUDEPATH += ../src
SOURCES += HkxReaderTests.cpp ../src/HkxReader.cpp
HEADERS += ../src/HkxReader.h

SOURCES += ../src/StudioPaths.cpp
