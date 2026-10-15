QT = core
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = csv_compatibility
isEmpty(RESVG_SUITE): error("Pass RESVG_SUITE pointing to the patched suite")
INCLUDEPATH += $$RESVG_SUITE/tools/vdiff/src
SOURCES += $$RESVG_SUITE/tools/vdiff/src/settings.cpp
NO_EXCEPTION_SOURCES += $$PWD/CsvCompatibility_tests.cpp $$RESVG_SUITE/tools/vdiff/src/tests.cpp
DEFINES += SRCDIR=\\\"$$RESVG_SUITE/tools/vdiff/\\\"

include($$RESVG_SUITE/tools/vdiff/no_exceptions.pri)
