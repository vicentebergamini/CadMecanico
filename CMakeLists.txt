cmake_minimum_required(VERSION 3.21)
project(CadMecanico VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CADMECANICO_BUILD_TESTS "Compila os testes automatizados (CTest)" ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(OpenCASCADE CONFIG REQUIRED)

# Bibliotecas sem Qt
add_subdirectory(src/modeling)
add_subdirectory(src/io)

qt_add_executable(CadMecanico src/main.cpp)
set_target_properties(CadMecanico PROPERTIES AUTOMOC ON)

target_link_libraries(CadMecanico PRIVATE
    cad_modeling
    cad_io
    Qt6::Widgets
    Qt6::OpenGLWidgets
    TKernel TKMath TKG3d TKBRep TKTopAlgo TKPrim TKBO
    TKService TKV3d TKOpenGl
    TKDESTEP TKXSBase
)

if(CADMECANICO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
