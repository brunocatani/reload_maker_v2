QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = GrabLocationsTests
INCLUDEPATH += ../src
SOURCES += GrabLocationsTests.cpp ../src/OperatingGrabLocations.cpp
