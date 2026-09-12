include_directories("include")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

add_library(${PROJECT_NAME} SHARED 
    "src/extension.cpp"
    "src/spiration/python/executor.cpp"
)

target_link_libraries(${PROJECT_NAME} PUBLIC 
    Python3::Python
    pybind11::headers
)

add_library(${PROJECT_NAME}.bootstrap SHARED 
    "src/bootstrap.cpp"
)

add_library(spiration::python ALIAS ${PROJECT_NAME})
if(UNIX)
    target_link_options(${PROJECT_NAME} PRIVATE 
        "LINKER:--unresolved-symbols=ignore-in-shared-lib"
        #-Wl,--unresolved-symbols=ignore-in-shared-lib
    )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE SPIRATION_EXTENSION)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    spiration::api
)
