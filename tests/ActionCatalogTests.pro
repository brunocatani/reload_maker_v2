QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = ActionCatalogTests
INCLUDEPATH += ../src
SOURCES += ActionCatalogTests.cpp ../src/ActionCatalog.cpp
HEADERS += ../src/ActionCatalog.h
