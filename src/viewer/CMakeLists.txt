qt_add_library(cad_viewer STATIC
    OcctViewerWidget.h OcctViewerWidget.cpp
)
set_target_properties(cad_viewer PROPERTIES AUTOMOC ON)
target_include_directories(cad_viewer PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_viewer PUBLIC
    Qt6::Widgets
    Qt6::OpenGLWidgets
    TKernel TKMath TKBRep TKService TKV3d TKOpenGl
)
if(WIN32)
    target_link_libraries(cad_viewer PRIVATE opengl32)
endif()
