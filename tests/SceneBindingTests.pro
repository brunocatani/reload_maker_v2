QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = SceneBindingTests
INCLUDEPATH += ../src
SOURCES += SceneBindingTests.cpp ../src/ScenePoseBinding.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp ../src/AttachmentConnections.cpp

SOURCES += ../src/StudioPaths.cpp
