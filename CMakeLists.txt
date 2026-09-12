cmake_minimum_required(VERSION 3.14)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS deployment target")
endif()

project(spiration.python VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/ConfigureLocal.cmake OPTIONAL)
include(cmake/ConfigureCompiler.cmake)
include(cmake/ConfigureDependencies.cmake)
include(cmake/ConfigureLibrary.cmake)
