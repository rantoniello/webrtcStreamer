# CMake target version
cmake_minimum_required(VERSION 3.10)

# Project name and version
project(native2webrtc VERSION 1.0)

# Specify compilers and the C++ standard
set(CMAKE_CXX_COMPILER /usr/bin/g++)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Paths definitions
cmake_path(SET CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/_install_dir")
cmake_path(SET INCLUDEDIR "${CMAKE_INSTALL_PREFIX}/include")
cmake_path(SET LIBDIR "${CMAKE_INSTALL_PREFIX}/lib")
cmake_path(SET BINDIR "${CMAKE_INSTALL_PREFIX}/bin")
add_definitions(-DINSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}")
add_definitions(-DTESTS_PREFIX="${PROJECT_SOURCE_DIR}/src/tests")

# Create directory tree
file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
file(MAKE_DIRECTORY "${INCLUDEDIR}")

# Link directories
link_directories("${LIBDIR}" "${CMAKE_CURRENT_BINARY_DIR}/Release" 
    "${CMAKE_CURRENT_BINARY_DIR}/Debug")

##############################################################################
# Add 3rd party libraries
##############################################################################

include(ExternalProject)

#### json ####

ExternalProject_Add(json 
    PREFIX "${PROJECT_SOURCE_DIR}/_install_dir"
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/3rdplibs/json"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=ON
        -DJSON_BuildTests=OFF
)

#### openssl ####

ExternalProject_Add(openssl 
    PREFIX "${PROJECT_SOURCE_DIR}/_install_dir"
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/3rdplibs/openssl-cmake"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=ON
)

#### libwebsockets ####

ExternalProject_Add(libwebsockets 
    PREFIX "${PROJECT_SOURCE_DIR}/_install_dir"
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/3rdplibs/libwebsockets"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=ON
)

#### libdatachannel ####

ExternalProject_Add(libdatachannel-proj 
    PREFIX "${PROJECT_SOURCE_DIR}/_install_dir"
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/3rdplibs/libdatachannel"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_STATIC_LIBS=OFF 
        -DBUILD_SHARED_LIBS=ON
        -DOPENSSL_ROOT_DIR=${CMAKE_INSTALL_PREFIX}
)
add_dependencies(libdatachannel-proj libwebsockets openssl json)
add_library(libdatachannel SHARED IMPORTED)
set_target_properties(libdatachannel PROPERTIES IMPORTED_LOCATION 
    "${LIBDIR}/libdatachannel.so")

add_custom_command(
  TARGET libdatachannel-proj 
  COMMAND make -f "${PROJECT_SOURCE_DIR}/ffmpeg.mk" ffmpeg
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)

##############################################################################
# Program
##############################################################################

add_executable(streamer-example
    "${PROJECT_SOURCE_DIR}/src/utils/BlockingQueue.h"
    "${PROJECT_SOURCE_DIR}/src/utils/SafeUMap.h"
    "${PROJECT_SOURCE_DIR}/src/VideoProducer.cpp"
    "${PROJECT_SOURCE_DIR}/src/WebRTCStreamer.cpp"
    "${PROJECT_SOURCE_DIR}/src/main.cpp"
)
add_dependencies(streamer-example libdatachannel-proj)
target_link_libraries(streamer-example libdatachannel libwebsockets.so 
    libavutil.so libavdevice.so libavformat.so libavcodec.so)
target_include_directories(streamer-example PUBLIC
    $<BUILD_INTERFACE:${INCLUDEDIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/Release
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/Debug>
)
install(TARGETS streamer-example
    RUNTIME DESTINATION "${BINDIR}"
)

add_executable(wsserver
    "${PROJECT_SOURCE_DIR}/src/wsserver.cpp"
)
add_dependencies(wsserver libdatachannel-proj)
target_link_libraries(wsserver libwebsockets.so)
target_include_directories(wsserver PUBLIC
    $<BUILD_INTERFACE:${INCLUDEDIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/Release
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/Debug>
)
install(TARGETS wsserver
    RUNTIME DESTINATION "${BINDIR}"
)
