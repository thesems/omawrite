QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2 dbus

CONFIG += c++17 release
TARGET = omawrite-v2
TEMPLATE = app

HEADERS += \
    src/explorer.h \
    src/backend.h \
    src/markdownstructure.h \
    src/markdownhighlighter.h \
    src/systemtheme.h

SOURCES += \
    src/explorer.cpp \
    src/main.cpp \
    src/backend.cpp \
    src/markdownhighlighter.cpp \
    src/systemtheme.cpp

RESOURCES += src/resources.qrc
