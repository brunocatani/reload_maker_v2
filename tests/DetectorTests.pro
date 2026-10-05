QT = core
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = DetectorTests
INCLUDEPATH += ../src
SOURCES += DetectorTests.cpp ../src/domain/detectors/ClipEvents.cpp ../src/domain/detectors/PresetProposal.cpp ../src/domain/presets/WeaponPresets.cpp ../src/domain/weapon/WeaponModel.cpp
HEADERS += ../src/domain/detectors/ClipEvents.h ../src/domain/detectors/PresetProposal.h ../src/domain/presets/WeaponPresets.h ../src/domain/weapon/WeaponModel.h
