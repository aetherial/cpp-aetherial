#-------------------------------------------------
#
# Project created by QtCreator 2014-01-22T11:47:38
#
#-------------------------------------------------

QT       += core gui widgets

QMAKE_CXXFLAGS += -std=c++11

TARGET = alephzero
TEMPLATE = app

QMAKE_LIBDIR += ../../cpp-ethereum-build/libethereum ../../secp256k1 ../../cryptopp562

LIBS += -Wl,-rpath,../../cpp-ethereum-build/libethereum -Wl,-rpath,../../secp256k1 -Wl,-rpath,../../cryptopp562 -lethereum -lsecp256k1 -lleveldb -lcryptopp -lgmp -lboost_filesystem -lboost_system

SOURCES += main.cpp\
        Main.cpp

HEADERS  += Main.h

FORMS    += Main.ui

INCLUDEPATH = ../../secp256k1/include ../../cryptopp562 ../../cpp-ethereum


