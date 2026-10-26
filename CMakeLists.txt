cmake_minimum_required(VERSION 3.24)
project(SkyCounterUI VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep in sync with the CommonLibSSE-NG branch used by the SkyOS source tree.
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibSSE-NG.git
    GIT_TAG ng
    GIT_SUBMODULES ""
    GIT_SHALLOW TRUE
)

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE-NG tests" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable CommonLibSSE-NG tests" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "SkyCounter UI supports Skyrim SE/AE, not VR" FORCE)

# CommonLib is a build dependency; do not copy its install rules into the mod package.
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(CommonLibSSE)
set(CMAKE_SKIP_INSTALL_RULES OFF)
include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

find_package(directxtk CONFIG REQUIRED)

add_commonlibsse_plugin(SkyCounterUI
    NAME "SkyCounter UI"
    AUTHOR "SkyOS"
    VERSION ${PROJECT_VERSION}
    SOURCES
        src/skycounter_ui.cpp
        src/skycounter_locations.cpp
        src/skyitemicons.cpp
        src/skycounter_entry.cpp
)

target_include_directories(SkyCounterUI PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/SKSE/Plugins/SkyCounterUIAPIHeader/include"
)
target_compile_features(SkyCounterUI PRIVATE cxx_std_23)
target_compile_definitions(SkyCounterUI PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SKYOS_SKYCOUNTERUI_EXPORTS
)
target_precompile_headers(SkyCounterUI PRIVATE src/pch.h)
target_link_libraries(SkyCounterUI PRIVATE Windowscodecs Ole32 Microsoft::DirectXTK)

install(TARGETS SkyCounterUI RUNTIME DESTINATION "Data/SKSE/Plugins")

# The dashboard is hosted by SkyOS's shared PrismaUI view tree. The app owns
# its branding, module configuration, guides, and capture/cache destinations.
install(DIRECTORY data/PrismaUI/views/SkyOS/SkyCounter
    DESTINATION "Data/PrismaUI/views/SkyOS")
install(DIRECTORY data/PrismaUI/views/SkyCounterUI
    DESTINATION "Data/PrismaUI/views")
install(DIRECTORY data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI
    DESTINATION "Data/SKSE/Plugins/SkyOS/Mods")
install(DIRECTORY data/SKSE/Plugins/SkyOS/Mods/SkyViewer/SkyCounterUI
    DESTINATION "Data/SKSE/Plugins/SkyOS/Mods/SkyViewer")

# Ship the public read-only C ABI header and its branded SkyDocuments guide.
install(FILES
    data/SKSE/Plugins/SkyCounterUIAPIHeader/include/SkyCounterUIAPI.h
    DESTINATION "Data/SKSE/Plugins/SkyCounterUIAPIHeader/include")
install(FILES data/SKSE/Plugins/SkyCounterUIAPIHeader/README.md
    DESTINATION "Data/SKSE/Plugins/SkyCounterUIAPIHeader")
install(DIRECTORY data/SKSE/Plugins/SkyOS/Mods/SkyCounterUIAPIHeader
    DESTINATION "Data/SKSE/Plugins/SkyOS/Mods")

set(CPACK_PACKAGE_NAME "SkyCounterUI")
set(CPACK_PACKAGE_VENDOR "SkyOS")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "SkyCounterUI-${PROJECT_VERSION}-SkyrimSE-AE")
set(CPACK_GENERATOR "ZIP")
include(CPack)
