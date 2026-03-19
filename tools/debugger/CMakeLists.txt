cmake_minimum_required(VERSION 3.28)
project(pegasus_debugger)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Boost REQUIRED COMPONENTS serialization)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui")
set(PEGASUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(pegasus_debugger
    main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(pegasus_debugger PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PEGASUS_ROOT}
    ${SDL2_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(pegasus_debugger PRIVATE
    pegasuscosimlib
    pegasussim
    Boost::serialization
    SDL2::SDL2
    OpenGL::GL
    ${CMAKE_DL_LIBS}
)

# ── Symlinks so the debugger binary can find arch/ and mavis_json/ ──────────
file(CREATE_LINK ${PEGASUS_ROOT}/arch
     ${CMAKE_CURRENT_BINARY_DIR}/arch SYMBOLIC)

file(CREATE_LINK ${PEGASUS_ROOT}/mavis/json
     ${CMAKE_CURRENT_BINARY_DIR}/mavis_json SYMBOLIC)

file(CREATE_LINK ${PEGASUS_ROOT}/core/rv64
     ${CMAKE_CURRENT_BINARY_DIR}/rv64 SYMBOLIC)