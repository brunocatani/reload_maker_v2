QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = AuthoringTests
INCLUDEPATH += ../src
SOURCES += AuthoringTests.cpp ../src/CollisionDraftPolicy.cpp ../src/AuthoringPaths.cpp ../src/SourcePolicy.cpp
