include( $${TOP_SOURCE_DIR}/unittests/unittests.pri )
QT += network
SOURCES = webcalls.cpp
test.target = test
test.commands = ./$(TARGET)
test.depends = $(TARGET)
QMAKE_EXTRA_TARGETS += test
