QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = InstallationTests
INCLUDEPATH += ../src
SOURCES += InstallationTests.cpp ../src/InstallationSetup.cpp ../src/StudioPaths.cpp ../src/StudioSession.cpp
