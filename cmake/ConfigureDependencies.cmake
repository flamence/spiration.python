find_package(Python3 3.11.9 REQUIRED COMPONENTS Interpreter Development.Embed)

include(FetchContent)

FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v3.1.0
)
FetchContent_MakeAvailable(pybind11)

FetchContent_Declare(
    spiration.api
    GIT_REPOSITORY https://github.com/flamence/spiration.api.git
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(spiration.api)
