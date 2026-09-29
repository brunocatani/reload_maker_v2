QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = GripViewportTests
INCLUDEPATH += ../src
SOURCES += GripViewportTests.cpp ../src/GripViewportMath.cpp ../src/PartGrip.cpp ../src/HandPoseLibrary.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp

SOURCES += ../src/StudioPaths.cpp
