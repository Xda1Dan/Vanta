cmake_minimum_required(VERSION 3.25)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Target macOS architecture")
endif()

project(Vanta LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)

add_subdirectory(Dependencies/luau EXCLUDE_FROM_ALL)

add_library(tinyhook STATIC
    Dependencies/tinyhook/src/exhook.c
    Dependencies/tinyhook/src/interpose.c
    Dependencies/tinyhook/src/memory.c
    Dependencies/tinyhook/src/objcrt.c
    Dependencies/tinyhook/src/symbol.c
    Dependencies/tinyhook/src/tinyhook.c
)
target_include_directories(tinyhook
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/tinyhook/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/tinyhook/src
)

set(VANTA_CORE_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RBX/task_scheduler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RBX/security
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RBX/execution
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RBX/environment
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Roblox
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/managers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Utilitys
    ${CMAKE_CURRENT_SOURCE_DIR}/Dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/tinyhook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/lib/base64/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/lib/lz4/include
)

set(VANTA_DECOMPILER_LIB
    "${CMAKE_CURRENT_SOURCE_DIR}/../unix-roblox/decompiler/target/release/libnexa_decompiler.a"
    CACHE FILEPATH "Path to the Rust decompiler static archive")

add_library(Vanta SHARED
    src/main.cpp
    src/renderer/Overlay.mm
    src/managers/DecompilerShim.cpp
    Dependencies/lib/lz4/include/lz4.c
)

if(EXISTS "${VANTA_DECOMPILER_LIB}")
    target_compile_definitions(Vanta PRIVATE VANTA_HAVE_DECOMPILER=1)
endif()

target_include_directories(Vanta PRIVATE ${VANTA_CORE_INCLUDES})

target_link_libraries(Vanta
    PRIVATE
        Luau.Compiler
        Luau.VM
        tinyhook
        objc
        curl
        "-framework ApplicationServices"
        "-framework Cocoa"
        "-framework CoreFoundation"
)

if(EXISTS "${VANTA_DECOMPILER_LIB}")
    target_link_libraries(Vanta PRIVATE "${VANTA_DECOMPILER_LIB}"
        "-framework Security"
        "-framework CoreFoundation"
        resolv
    )
endif()

set_target_properties(Vanta PROPERTIES OUTPUT_NAME "Vanta")
target_compile_options(Vanta PRIVATE -ffile-prefix-map=${CMAKE_SOURCE_DIR}=)
target_compile_options(tinyhook PRIVATE -ffile-prefix-map=${CMAKE_SOURCE_DIR}=)

add_executable(vanta-machopatch tools/machopatch.c)
set_target_properties(vanta-machopatch PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    OUTPUT_NAME "vanta-machopatch"
)

add_executable(vanta-luaucheck tools/luaucheck.cpp)
target_include_directories(vanta-luaucheck PRIVATE ${VANTA_CORE_INCLUDES})
target_link_libraries(vanta-luaucheck PRIVATE Luau.Compiler)
set_target_properties(vanta-luaucheck PROPERTIES
    CXX_STANDARD 20
    OUTPUT_NAME "vanta-luaucheck"
)
