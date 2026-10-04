QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = WeaponModelTests
INCLUDEPATH += ../src
SOURCES += WeaponModelTests.cpp ../src/domain/weapon/WeaponModel.cpp
HEADERS += ../src/domain/weapon/WeaponModel.h
