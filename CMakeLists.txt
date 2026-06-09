cmake_minimum_required(VERSION 3.10)
project(fpv_osd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_library(PIGPIO_LIB pigpio REQUIRED)

# libcamera
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# EGL, GLES, GBM, DRM
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(DRM REQUIRED libdrm)

add_subdirectory(src/imgui)

include_directories(
    ${LIBCAMERA_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLESV2_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
)

link_directories(
    ${LIBCAMERA_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GLESV2_LIBRARY_DIRS}
    ${GBM_LIBRARY_DIRS}
    ${DRM_LIBRARY_DIRS}
)

add_executable(fpv_osd src/main.cpp
                        src/camera.cpp
                        src/shaders.cpp
                        src/instruments.cpp)

target_link_libraries(fpv_osd
    ${LIBCAMERA_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLESV2_LIBRARIES}
    ${GBM_LIBRARIES}
    ${DRM_LIBRARIES}
    ${GLESv2_LIBRARY}
    imgui
    pthread
    rt
    pigpio
)
