TEMPLATE = app
TARGET = netvirt
QT += quick xml androidextras

ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android

SOURCES = main.cpp
HEADERS = logging.h

SOURCES += agent.cpp service.cpp service_main.cpp
HEADERS += agent.h    service.h  service_main.h

RESOURCES += \
    netvirt.qrc

OTHER_FILES = \
    $$files(*.qml) \
    android/AndroidManifest.xml\
    android/src/com/netvirt/netvirt/NetvirtAgent.java \
    android/src/com/netvirt/netvirt/ToyVpnService.java \
    android/src/com/netvirt/netvirt/ToyVpnServiceQt.java
