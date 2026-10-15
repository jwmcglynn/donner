QT = core gui
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = canonical-backend-tests
isEmpty(RESVG_SUITE): error("Pass RESVG_SUITE pointing to the patched suite")
INCLUDEPATH += $$RESVG_SUITE/tools/vdiff/src
NO_EXCEPTION_SOURCES += $$PWD/CanonicalBackend_tests.cpp \
    $$RESVG_SUITE/tools/vdiff/src/donner-renderer.cpp
DEFINES += SRCDIR=\\\"$$RESVG_SUITE/tools/vdiff/\\\"
include($$RESVG_SUITE/tools/vdiff/no_exceptions.pri)
