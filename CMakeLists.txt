cmake_minimum_required(VERSION 3.10)

project(FastCgi)

ADD_DEFINITIONS(-DUNICODE)
ADD_DEFINITIONS(-D_UNICODE)

# specify the C++ standard
set(SUPPORTED_CXX_STANDARDS 14 17)
if(NOT DEFINED CMAKE_CXX_STANDARD)
  message(STATUS "Setting C++ version to '14' as none was specified.")
  set(CMAKE_CXX_STANDARD 14)
else()
  message(STATUS "${PROJECT_NAME}: CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD} set by parent project.")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

string(FIND "${CMAKE_CXX_FLAGS_DEBUG}" "-O0" nPos)
if(${nPos} EQUAL "-1")
  SET(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3 -ggdb -D_DEBUG -DDEBUG")
else()
  message(STATUS "${PROJECT_NAME}: CMAKE_CXX_FLAGS_DEBUG hat bereits -O0 enthalten")
endif()

include_directories(${CMAKE_SOURCE_DIR})

set(targetSrc
        ${CMAKE_CURRENT_LIST_DIR}/FastCgi.cpp
)

add_library(FastCgi STATIC ${targetSrc})

install(TARGETS FastCgi DESTINATION lib)
#install(FILES SocketLib.h DESTINATION include)
