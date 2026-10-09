QT = core gui
CONFIG += console c++20 release
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = AmmunitionTests
INCLUDEPATH += ../src
SOURCES += AmmunitionTests.cpp ../src/WeaponAmmunition.cpp ../src/WeaponConfiguration.cpp ../src/EspReader.cpp ../src/SourcePolicy.cpp
RESOURCES += ../assets/source-policies.qrc
