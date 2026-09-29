QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = PartGripTests
INCLUDEPATH += ../src
SOURCES += PartGripTests.cpp ../src/PartGrip.cpp ../src/HandPoseLibrary.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp ../src/ScenePoseBinding.cpp ../src/MechanismScene.cpp

SOURCES += ../src/StudioPaths.cpp
