cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-fdiagnostics-color=always)

# Demo builds: TAMA_TIME_SCALE=60 idf.py build  ->  1 real minute = 1 pet-hour
# (tama_main.c defaults the macro to 1 = real time).
if(DEFINED ENV{TAMA_TIME_SCALE})
    add_compile_definitions(TAMA_TIME_SCALE=$ENV{TAMA_TIME_SCALE})
endif()

project(watcher_os)
