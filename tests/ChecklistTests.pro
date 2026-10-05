QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = ChecklistTests
INCLUDEPATH += ../src
SOURCES += ChecklistTests.cpp ../src/domain/checklist/Checklist.cpp ../src/domain/weapon/WeaponModel.cpp ../src/domain/presets/WeaponPresets.cpp
HEADERS += ../src/domain/checklist/Checklist.h ../src/domain/weapon/WeaponModel.h ../src/domain/presets/WeaponPresets.h
