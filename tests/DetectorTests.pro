QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = DetectorTests
INCLUDEPATH += ../src
SOURCES += DetectorTests.cpp ../src/domain/detectors/ClipEvents.cpp
HEADERS += ../src/domain/detectors/ClipEvents.h
