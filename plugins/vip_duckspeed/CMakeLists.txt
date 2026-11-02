# vip_duckspeed — Plugify C++ plugin: port of the Pisex VIP module "[VIP] DuckSpeed".
#
# Pure s2sdk: no s2guard and no pisexcompat (docs/PLUGIN_STANDARD.md §7). Every safety rule the
# module used to get from pisexcompat (the map gate, the slot/controller/pawn checks, the
# map-gated schema offsets) lives in src/plugin.cpp.
#
#   plugify-vip_duckspeed  the plugin shared library (entry "bin/vip_duckspeed" -> bin/libvip_duckspeed.so)
#   vip_duckspeed_tests    engine-free unit tests of the per-slot bookkeeping (build/tests)

set(VIP_DUCKSPEED_VERSION "1.2.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_duckspeed"), so vip_bindings already
# exists; a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has
# no plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_duckspeed SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_duckspeed PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_duckspeed SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_duckspeed PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_duckspeed PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_duckspeed PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_duckspeed"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_duckspeed PROPERTIES OUTPUT_NAME vip_duckspeed)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_duckspeed PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_duckspeed.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_duckspeed.pplugin"
)

# --- tests --------------------------------------------------------------------
# Engine-free logic tests; run by ./test.sh (build/tests). They include only
# src/duckspeed_logic.hpp, so they carry no s2sdk, vip or pisexcompat dependency at all.
add_executable(vip_duckspeed_tests tests/vip_duckspeed_tests.cpp)
target_include_directories(vip_duckspeed_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_duckspeed_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_duckspeed_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_duckspeed_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_duckspeed_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Runtime tests: compile ../src/plugin.cpp with the real generated s2sdk/vip bindings.
add_executable(vip_duckspeed_runtime_tests
        tests/vip_duckspeed_runtime_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(vip_duckspeed_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_duckspeed_runtime_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_duckspeed_runtime_tests PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_duckspeed_runtime_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_duckspeed_runtime_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_duckspeed_runtime_tests"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(vip_duckspeed_runtime_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
