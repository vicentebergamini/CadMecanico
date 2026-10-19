cmake_minimum_required(VERSION 3.21)
project(CadMecanico VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(OpenCASCADE CONFIG REQUIRED)

qt_add_executable(CadMecanico src/main.cpp)

target_link_libraries(CadMecanico PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    TKernel TKMath TKG3d TKBRep TKTopAlgo TKPrim TKBO
    TKService TKV3d TKOpenGl
    TKDESTEP TKXSBase
)
