cmake_minimum_required(VERSION 3.14)

set(SPIRATION_PYTHON_VERSION "3.11.9" CACHE STRING "Required CPython version")

find_package(Python3 ${SPIRATION_PYTHON_VERSION} EXACT REQUIRED
    COMPONENTS Interpreter Development.Embed)

set(SPIRATION_PYTHON_PREFIX "${Python3_ROOT_DIR}")
if(NOT SPIRATION_PYTHON_PREFIX AND Python3_EXECUTABLE)
    get_filename_component(SPIRATION_PYTHON_PREFIX "${Python3_EXECUTABLE}" DIRECTORY)
endif()
file(TO_CMAKE_PATH "${SPIRATION_PYTHON_PREFIX}" SPIRATION_PYTHON_PREFIX)

if(SPIRATION_PYTHON_PREFIX AND NOT EXISTS "${SPIRATION_PYTHON_PREFIX}/Lib/os.py")
    message(WARNING
        "spiration.python: ${SPIRATION_PYTHON_PREFIX} does not look like a "
        "usable Python root (missing Lib/os.py). The build-time fallback will "
        "be rejected at run time; set -DPython3_ROOT_DIR=<real install>.")
endif()

message(STATUS
    "spiration.python: CPython ${Python3_VERSION} root ${SPIRATION_PYTHON_PREFIX}")

include(FetchContent)
FetchContent_Declare(
    spiration.api
    GIT_REPOSITORY https://github.com/flamence/spiration.api.git
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(spiration.api)
