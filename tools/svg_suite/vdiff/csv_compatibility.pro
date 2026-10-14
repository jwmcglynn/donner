QT = core
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = csv_compatibility
isEmpty(RESVG_SUITE): error("Pass RESVG_SUITE pointing to the patched suite")
INCLUDEPATH += $$RESVG_SUITE/tools/vdiff/src
SOURCES += $$PWD/CsvCompatibility_tests.cpp \
    $$RESVG_SUITE/tools/vdiff/src/tests.cpp \
    $$RESVG_SUITE/tools/vdiff/src/settings.cpp
DEFINES += SRCDIR=\\\"$$RESVG_SUITE/tools/vdiff/\\\"
