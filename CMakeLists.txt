cmake_minimum_required(VERSION 3.10)

project(FastCgi)

ADD_DEFINITIONS(-DUNICODE)
ADD_DEFINITIONS(-D_UNICODE)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(${CMAKE_SOURCE_DIR})

set(targetSrc
        ${CMAKE_CURRENT_LIST_DIR}/FastCgi.cpp
)

add_library(FastCgi STATIC ${targetSrc})

install(TARGETS FastCgi DESTINATION lib)
#install(FILES SocketLib.h DESTINATION include)
