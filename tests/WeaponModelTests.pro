QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = WeaponModelTests
INCLUDEPATH += ../src
SOURCES += WeaponModelTests.cpp ../src/domain/weapon/WeaponModel.cpp ../src/domain/weapon/WeaponOverrides.cpp ../src/domain/presets/WeaponPresets.cpp
HEADERS += ../src/domain/weapon/WeaponModel.h ../src/domain/weapon/WeaponOverrides.h ../src/domain/presets/WeaponPresets.h
