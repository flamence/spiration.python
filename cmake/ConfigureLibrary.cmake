add_library(${PROJECT_NAME} SHARED
    "src/spiration/python/loader.cpp"
    "src/spiration/python/module.cpp"
    "src/spiration/python/runtime.cpp"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE SPIRATION_EXTENSION)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SPIRATION_PYTHON_VERSION_STRING="${Python3_VERSION}"
    SPIRATION_PYTHON_VERSION_COMPACT="${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    spiration::api
    Python3::Python
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE advapi32)
endif()

if(SPIRATION_PYTHON_PREFIX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        SPIRATION_PYTHON_DEFAULT_HOME="${SPIRATION_PYTHON_PREFIX}"
    )
endif()

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

if(WIN32)
    get_filename_component(_spiration_python_bin "${Python3_EXECUTABLE}" DIRECTORY)
    file(GLOB _spiration_python_dlls "${_spiration_python_bin}/python3*.dll")
    if(_spiration_python_dlls)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_spiration_python_dlls}
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
        install(FILES ${_spiration_python_dlls} DESTINATION "bin")
    endif()
endif()
