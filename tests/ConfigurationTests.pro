QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = ConfigurationTests
INCLUDEPATH += ../src
SOURCES += ConfigurationTests.cpp ../src/WeaponConfiguration.cpp ../src/EspReader.cpp ../src/SlotOverlayLayout.cpp

SOURCES += ../src/StudioPaths.cpp
