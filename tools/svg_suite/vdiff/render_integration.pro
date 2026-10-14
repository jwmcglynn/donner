QT = core gui widgets concurrent sql
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = render_integration
isEmpty(RESVG_SUITE): error("Pass RESVG_SUITE pointing to the patched suite")
INCLUDEPATH += $$RESVG_SUITE/tools/vdiff/src
SOURCES += $$PWD/RenderIntegration_tests.cpp \
    $$RESVG_SUITE/tools/vdiff/src/tests.cpp \
    $$RESVG_SUITE/tools/vdiff/src/settings.cpp
DEFINES += SRCDIR=\\\"$$RESVG_SUITE/tools/vdiff/\\\"

SOURCES += $$RESVG_SUITE/tools/vdiff/src/render.cpp \
    $$RESVG_SUITE/tools/vdiff/src/process.cpp \
    $$RESVG_SUITE/tools/vdiff/src/paths.cpp \
    $$RESVG_SUITE/tools/vdiff/src/imagecache.cpp \
    $$RESVG_SUITE/tools/vdiff/src/exportdialog.cpp
HEADERS += $$RESVG_SUITE/tools/vdiff/src/render.h $$RESVG_SUITE/tools/vdiff/src/exportdialog.h
FORMS += $$RESVG_SUITE/tools/vdiff/src/exportdialog.ui
