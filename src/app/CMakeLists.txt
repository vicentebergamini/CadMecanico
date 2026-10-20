qt_add_library(cad_app STATIC
    MainWindow.h MainWindow.cpp
)
set_target_properties(cad_app PROPERTIES AUTOMOC ON)
target_include_directories(cad_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_app PUBLIC
    Qt6::Widgets
    cad_modeling
    cad_io
)
