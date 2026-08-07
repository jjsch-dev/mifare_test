# mifare_test — Stage 2 console test harness for the mifare component
# (MFRC522-compatible reader: NZ3801-AB on the lock, RC522 module on the bench)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mifare_test)
