cmake_minimum_required(VERSION 3.9)
set(LIBNAME mylib)
project(${LIBNAME} LANGUAGES C CXX VERSION 1.0.1 DESCRIPTION "Shared library")

add_library(${LIBNAME} SHARED 
    src/nofile.cpp
    )
set_target_properties(${LIBNAME} PROPERTIES
    SOVERSION 1
    PUBLIC_HEADER src/nofile.h
    LINKER_LANGUAGE CXX)
#target_link_libraries(${LIBNAME} )
target_include_directories(${LIBNAME} PRIVATE src)
