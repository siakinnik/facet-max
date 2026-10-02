cmake_minimum_required(VERSION 3.18)
project(facet-max VERSION 0.1.0 LANGUAGES C CXX)
set(MAX_VERSION_SUFFIX "alpha")  # pre-release tag; empty for final releases; keep manifest.json in sync

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MAX_STATIC "Link fully static (portable release binaries)" OFF)
option(MAX_RUNTIME "Build the bundled runtime (libraries, dbus-daemon, Qt plugin, fonts)" ON)

if(MAX_VERSION_SUFFIX)
    set(MAX_MODULE_VERSION "${PROJECT_VERSION}-${MAX_VERSION_SUFFIX}")
else()
    set(MAX_MODULE_VERSION "${PROJECT_VERSION}")
endif()

# The plugin SDK lives in facet-core: a local checkout next to this project
# (or FACET_CORE_DIR) is used when present, otherwise it is downloaded.
# Lent surfaces and capability endpoints need facet-core 0.6.
set(FACET_CORE_DIR "" CACHE PATH "facet-core checkout providing the SDK (default: ../facet-core, else download)")
set(FACET_CORE_REF "v0.6.0-alpha" CACHE STRING "facet-core git ref to download the SDK from")
if(NOT FACET_CORE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core/sdk/CMakeLists.txt")
    set(FACET_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core")
endif()
if(FACET_CORE_DIR)
    add_subdirectory(${FACET_CORE_DIR}/sdk ${CMAKE_BINARY_DIR}/facet_sdk)
else()
    include(FetchContent)
    FetchContent_Declare(facet_core
        GIT_REPOSITORY https://github.com/siakinnik/facet-core.git
        GIT_TAG ${FACET_CORE_REF}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk)
    FetchContent_MakeAvailable(facet_core)
endif()

# OpenSSL (downloads) and liblzma (.deb data) are linked statically.
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)
find_library(LZMA_STATIC NAMES liblzma.a lzma REQUIRED)
find_package(Threads REQUIRED)

add_library(max_core STATIC src/dbus.cpp src/secrets.cpp src/notify.cpp src/package.cpp src/https.cpp
                            src/installer.cpp src/session.cpp)
target_include_directories(max_core PUBLIC src)
target_compile_options(max_core PRIVATE -Wall -Wextra)
target_link_libraries(max_core PUBLIC facet_sdk OpenSSL::SSL OpenSSL::Crypto ${LZMA_STATIC} Threads::Threads ${CMAKE_DL_LIBS})

set(PLUGIN_OUT ${CMAKE_BINARY_DIR}/max.plugin)
add_executable(max src/main.cpp src/i18n/ru.cpp)
target_link_libraries(max PRIVATE max_core)
target_compile_options(max PRIVATE -Wall -Wextra)
target_compile_definitions(max PRIVATE MAX_MODULE_VERSION="${MAX_MODULE_VERSION}")
set_target_properties(max PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PLUGIN_OUT})
if(MAX_STATIC)
    target_link_options(max PRIVATE -static -s)
endif()

# Preloaded into MAX: makes its Qt use Wayland (see shim/shim.c).
add_library(facet-max-shim SHARED shim/shim.c)
target_link_libraries(facet-max-shim PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(facet-max-shim PRIVATE -Wall -Wextra -O2)
set_target_properties(facet-max-shim PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${PLUGIN_OUT}/runtime/lib)

# The build dir doubles as a plugin dir: FACET_PLUGIN_PATH=<build>/max.plugin
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${PLUGIN_OUT}/manifest.json)
if(MAX_RUNTIME)
    set(MAX_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/runtime" CACHE PATH "Where the bundled runtime is built")
    execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-runtime.sh" "${MAX_RUNTIME_DIR}"
                    RESULT_VARIABLE runtime_result)
    if(NOT runtime_result EQUAL 0)
        message(FATAL_ERROR "Building the runtime failed (scripts/build-runtime.sh)")
    endif()
    file(COPY ${MAX_RUNTIME_DIR}/ DESTINATION ${PLUGIN_OUT}/runtime PATTERN ".runtime" EXCLUDE)
endif()

enable_testing()
add_executable(max_test tests/max_test.cpp)
target_link_libraries(max_test PRIVATE max_core)
add_test(NAME max COMMAND max_test)
