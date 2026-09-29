QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = PoseTests
INCLUDEPATH += ../src
SOURCES += PoseTests.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp

SOURCES += ../src/StudioPaths.cpp
