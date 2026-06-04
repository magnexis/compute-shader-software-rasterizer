cmake_minimum_required(VERSION 3.24)
project(woah_man LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WOAH_MAN_BUILD_DEMO "Build the Win32/D3D12 demo application." ON)
option(WOAH_MAN_BUILD_DOCS "Build Doxygen documentation if Doxygen is available." OFF)

set(WOAH_MAN_RUNTIME_SOURCE_FILES
    src/main.cpp
    src/DemoApp.cpp
    src/DemoApp.h
    src/SoftwareRasterizerD3D12.cpp
    src/SoftwareRasterizerD3D12.h
)

set(WOAH_MAN_PACKAGE_FILES
    ${WOAH_MAN_RUNTIME_SOURCE_FILES}
    shaders/ComputeSoftwareRasterizer.hlsl
)

if (WIN32 AND WOAH_MAN_BUILD_DEMO)
    add_executable(woah_man WIN32 ${WOAH_MAN_RUNTIME_SOURCE_FILES})
    target_include_directories(woah_man PRIVATE src)
    target_link_libraries(woah_man
        d3d12
        dxcompiler
        dxgi
        dxguid
    )
elseif (WOAH_MAN_BUILD_DEMO)
    message(WARNING "WOAH_MAN_BUILD_DEMO is ON, but the demo currently requires Win32/D3D12. Building source-only utility target instead.")
    add_custom_target(woah_man ALL SOURCES ${WOAH_MAN_PACKAGE_FILES} README.md todo .gitignore)
else()
    add_custom_target(woah_man_assets ALL
        SOURCES ${WOAH_MAN_PACKAGE_FILES} README.md todo .gitignore
    )
endif()

install(FILES
    README.md
    todo
    .gitignore
    DESTINATION .
)

install(DIRECTORY shaders DESTINATION .)
install(DIRECTORY src DESTINATION . FILES_MATCHING PATTERN "*.h" PATTERN "*.cpp")

if (WOAH_MAN_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if (DOXYGEN_FOUND)
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
        set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/docs/html)
        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
    else()
        message(WARNING "Doxygen not found, docs target will not be generated.")
    endif()
endif()
