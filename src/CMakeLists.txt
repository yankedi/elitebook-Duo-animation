# ---------------------------------------------------------------------------
#  Dragonfly Sensor Diagnostic   (Milestone 1 - 3)
#
#  Target machine : HP Elite Dragonfly G2 / Windows 11 x64
#  Toolchain      : Visual Studio 2022 (MSVC v143) + Windows 10/11 SDK + CMake
#
#  This target is deliberately standalone:
#    * no NuGet, no cppwinrt.exe code generation
#    * C++/WinRT projections are consumed straight from the Windows SDK
#      (Include/<ver>/cppwinrt/winrt/*.h), which is all a consumer needs
#    * native HID / SetupAPI is used as a second, independent sensor channel
#
#  Configure:  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#  Build    :  cmake --build build --config Release
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

project(DragonflySensorDiag
        VERSION 0.1.0
        DESCRIPTION "HP Elite Dragonfly G2 hinge / sensor diagnostic"
        LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR
        "This project requires MSVC (Visual Studio 2022 Build Tools or IDE). "
        "Configure with: cmake -S . -B build -G \"Visual Studio 17 2022\" -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
#  Locate the Windows SDK so that <winrt/...> headers resolve even when the
#  MSBuild default include paths do not carry the cppwinrt projection folder.
# ---------------------------------------------------------------------------
set(_sdk_roots "")

if(DEFINED ENV{WindowsSdkDir} AND EXISTS "$ENV{WindowsSdkDir}")
    list(APPEND _sdk_roots "$ENV{WindowsSdkDir}")
endif()

foreach(_regkey
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]")
    get_filename_component(_root "${_regkey}" ABSOLUTE)
    if(EXISTS "${_root}")
        list(APPEND _sdk_roots "${_root}")
    endif()
endforeach()

list(REMOVE_DUPLICATES _sdk_roots)

set(DragonflyWindowsSdkRoot "" CACHE PATH
    "Windows SDK root (folder that contains Include/<version>/cppwinrt)")

if(NOT DragonflyWindowsSdkRoot)
    foreach(_root IN LISTS _sdk_roots)
        if(NOT EXISTS "${_root}/Include")
            continue()
        endif()
        file(GLOB _versions RELATIVE "${_root}/Include" "${_root}/Include/10.*")
        list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
        foreach(_v IN LISTS _versions)
            if(EXISTS "${_root}/Include/${_v}/cppwinrt/winrt/base.h")
                set(DragonflyWindowsSdkRoot "${_root}" CACHE PATH "" FORCE)
                set(DragonflyWindowsSdkVersion "${_v}" CACHE STRING "" FORCE)
                break()
            endif()
        endforeach()
        if(DragonflyWindowsSdkRoot)
            break()
        endif()
    endforeach()
endif()

if(NOT DragonflyWindowsSdkRoot)
    message(FATAL_ERROR
        "Windows SDK not found. Install the 'Windows 11 SDK' component, or pass "
        "-DDragonflyWindowsSdkRoot=\"C:/Program Files (x86)/Windows Kits/10\".")
endif()

if(NOT DragonflyWindowsSdkVersion)
    file(GLOB _versions RELATIVE "${DragonflyWindowsSdkRoot}/Include"
         "${DragonflyWindowsSdkRoot}/Include/10.*")
    list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
    foreach(_v IN LISTS _versions)
        if(EXISTS "${DragonflyWindowsSdkRoot}/Include/${_v}/cppwinrt/winrt/base.h")
            set(DragonflyWindowsSdkVersion "${_v}" CACHE STRING "" FORCE)
            break()
        endif()
    endforeach()
endif()

set(_cppwinrt_include "${DragonflyWindowsSdkRoot}/Include/${DragonflyWindowsSdkVersion}/cppwinrt")
set(_sdk_um_include "${DragonflyWindowsSdkRoot}/Include/${DragonflyWindowsSdkVersion}/um")
set(_sdk_shared_include "${DragonflyWindowsSdkRoot}/Include/${DragonflyWindowsSdkVersion}/shared")
set(_sdk_ucrt_include "${DragonflyWindowsSdkRoot}/Include/${DragonflyWindowsSdkVersion}/ucrt")
if(NOT EXISTS "${_cppwinrt_include}/winrt/base.h")
    message(FATAL_ERROR "cppwinrt projection headers missing under ${_cppwinrt_include}")
endif()

message(STATUS "Windows SDK .......... ${DragonflyWindowsSdkRoot} (${DragonflyWindowsSdkVersion})")
message(STATUS "C++/WinRT headers .... ${_cppwinrt_include}")

# ---------------------------------------------------------------------------
#  Target
# ---------------------------------------------------------------------------
add_executable(DragonflySensorDiag
    app/main.cpp
    app/ConsoleUi.cpp
    app/FoldEffectMode.cpp
    animation/FoldAnimationController.cpp
    capture/DesktopCapture.cpp
    config/ConfigLoader.cpp
    graphics/D3DDevice.cpp
    graphics/DebugFoldWindow.cpp
    graphics/FoldRenderer.cpp
    graphics/OrientationDemoWindow.cpp
    graphics/OverlayWindow.cpp
    hinge/HingeEstimator.cpp
    orientation/OrientationTracker.cpp
    sensors/SensorManager.cpp
    sensors/CustomSensorManager.cpp
    sensors/HidSensorEnumerator.cpp
    sensors/SensorLogger.cpp
)

target_include_directories(DragonflySensorDiag PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${_cppwinrt_include}"
    "${_sdk_um_include}"
    "${_sdk_shared_include}"
    "${_sdk_ucrt_include}"
)

target_compile_definitions(DragonflySensorDiag PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_compile_options(DragonflySensorDiag PRIVATE
    /utf-8
    /W4
    /permissive-
    /Zc:__cplusplus
    /EHsc
    /MP
)

target_link_libraries(DragonflySensorDiag PRIVATE
    windowsapp      # C++/WinRT umbrella import library
    setupapi
    hid
    cfgmgr32
    propsys
    ole32
    oleaut32
    d3d11           # debug fold window
    dxgi
    d3dcompiler     # runtime HLSL compilation for the debug shader
)

# Keep a copy of the estimator configuration next to the executable so the
# running binary and the source tree cannot drift apart.
add_custom_command(TARGET DragonflySensorDiag POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:DragonflySensorDiag>/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/config.json"
            "$<TARGET_FILE_DIR:DragonflySensorDiag>/config/config.json"
    COMMENT "Copying config.json next to the executable")

set_target_properties(DragonflySensorDiag PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
