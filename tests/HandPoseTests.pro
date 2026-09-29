QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = HandPoseTests
INCLUDEPATH += ../src
SOURCES += HandPoseTests.cpp ../src/HandPoseLibrary.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp ../src/ScenePoseBinding.cpp

SOURCES += ../src/StudioPaths.cpp
