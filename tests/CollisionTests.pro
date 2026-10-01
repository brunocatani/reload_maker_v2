QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = CollisionTests
INCLUDEPATH += ../src ../build-fast/native-engine ../../../PROJECT_ROCK_V2/main_projects/ROCK/src
SOURCES += CollisionTests.cpp ../src/CollisionAuthoring.cpp

INCLUDEPATH += ../build-fast/native-engine/lib/qhull/src
SOURCES += $$files($$PWD/../build-fast/native-engine/lib/qhull/src/libqhull_r/*.c)
