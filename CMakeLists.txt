cmake_minimum_required(VERSION 3.21)

project(NFSHeatVR VERSION 0.2.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A release ZIP must run on a clean Windows installation without requiring
# the user to separately install the Visual C++ runtime.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(NFSHEAT_VR_MINHOOK_DIR "" CACHE PATH "Path to a checkout of TsudaKageyu/minhook")
set(NFSHEAT_VR_OPENXR_DIR "" CACHE PATH "Path to a checkout of KhronosGroup/OpenXR-SDK")

if(NOT EXISTS "${NFSHEAT_VR_MINHOOK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set NFSHEAT_VR_MINHOOK_DIR to the MinHook v1.3.4 checkout. Run build.ps1 to prepare it.")
endif()
if(NOT EXISTS "${NFSHEAT_VR_OPENXR_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set NFSHEAT_VR_OPENXR_DIR to the OpenXR-SDK release-1.1.60 checkout. Run build.ps1 to prepare it.")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${NFSHEAT_VR_MINHOOK_DIR}" "${CMAKE_BINARY_DIR}/minhook")

# We statically link the official loader into our injected runtime. The user's
# active OpenXR runtime (Pimax in this installation) remains selected by Windows.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
add_subdirectory("${NFSHEAT_VR_OPENXR_DIR}" "${CMAKE_BINARY_DIR}/openxr")

# The OpenXR SDK defaults its static loader to the dynamic CRT on Windows.
# Keep it consistent with the runtime above so the shipped DLL has no VC++
# redistributable dependency.
if(MSVC)
    set_property(TARGET openxr_loader PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(NFSHeatVRRuntime SHARED
    src/runtime/runtime_main.cpp
    src/runtime/dxgi_hooks.cpp
    src/runtime/camera_probe.cpp
    src/runtime/depth_observer.cpp
    src/runtime/frostbite_camera.cpp
    src/runtime/input_bridge.cpp
    src/runtime/virtual_display.cpp
    src/runtime/logger.cpp
    src/runtime/vr_presenter.cpp
    src/shared/vr_settings.cpp
)

target_compile_definitions(NFSHeatVRRuntime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    XR_USE_PLATFORM_WIN32
    XR_USE_GRAPHICS_API_D3D11
    UNICODE
    _UNICODE
)

target_include_directories(NFSHeatVRRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared
)

target_link_libraries(NFSHeatVRRuntime PRIVATE
    minhook
    OpenXR::openxr_loader
    d3d11
    dxgi
    d3dcompiler
    shlwapi
    shell32
    ole32
    user32
)

set_target_properties(NFSHeatVRRuntime PROPERTIES
    OUTPUT_NAME "NFSHeatVRRuntime"
)

set(NFSHEAT_VR_ICON_RESOURCE src/resources/nfs_heat_vr.rc)

add_executable(NFSHeatVRLauncher src/launcher/launcher_main.cpp ${NFSHEAT_VR_ICON_RESOURCE})
target_compile_definitions(NFSHeatVRLauncher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(NFSHeatVRLauncher PRIVATE shlwapi shell32 ole32)
set_target_properties(NFSHeatVRLauncher PROPERTIES OUTPUT_NAME "NFSHeatVRLauncher")

add_executable(NFSHeatVRControl WIN32
    src/control/control_main.cpp
    src/shared/vr_settings.cpp
    ${NFSHEAT_VR_ICON_RESOURCE}
)
target_compile_definitions(NFSHeatVRControl PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(NFSHeatVRControl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/shared)
target_link_libraries(NFSHeatVRControl PRIVATE comctl32 shell32 ole32 uxtheme)
set_target_properties(NFSHeatVRControl PROPERTIES OUTPUT_NAME "NFSHeatVRControl")

install(TARGETS NFSHeatVRRuntime NFSHeatVRLauncher NFSHeatVRControl RUNTIME DESTINATION .)
