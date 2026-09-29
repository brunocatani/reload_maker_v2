QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = SimulationTests
INCLUDEPATH += ../src
SOURCES += SimulationTests.cpp ../src/WeaponSimulation.cpp ../src/ReloadRecipe.cpp ../src/PartGrip.cpp ../src/HandPoseLibrary.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp ../src/StudioPaths.cpp
