QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = ReloadRecipeTests
INCLUDEPATH += ../src
SOURCES += ReloadRecipeTests.cpp ../src/ReloadRecipe.cpp ../src/ReloadDriveMath.cpp ../src/PartGrip.cpp ../src/HandPoseLibrary.cpp ../src/PoseEvaluator.cpp ../src/HkxReader.cpp

SOURCES += ../src/StudioPaths.cpp
SOURCES += ../src/ReloadBundle.cpp ../src/PartMechanism.cpp
