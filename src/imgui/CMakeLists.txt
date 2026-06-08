cmake_minimum_required(VERSION 3.28)

project(imgui)

add_library(imgui SHARED src/imgui.cpp
                        src/imgui_demo.cpp
                        src/imgui_tables.cpp
                        src/imgui_widgets.cpp
                        src/imgui_draw.cpp
                        backends/imgui_impl_opengl3.cpp)

target_include_directories(imgui PUBLIC src backends)
