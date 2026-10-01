QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = ScopeProfileTests
INCLUDEPATH += ../src
SOURCES += ScopeProfileTests.cpp ../src/ScopeProfile.cpp
