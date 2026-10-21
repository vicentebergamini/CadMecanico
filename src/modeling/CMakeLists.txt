add_library(cad_modeling STATIC
    TestPart.h TestPart.cpp
    ShapeInspector.h ShapeInspector.cpp
    SubShapeInfo.h SubShapeInfo.cpp
    Features.h Features.cpp
    EdgeNaming.h EdgeNaming.cpp
)
target_include_directories(cad_modeling PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_modeling PUBLIC
    TKernel TKMath TKG2d TKG3d TKGeomBase TKGeomAlgo TKBRep TKTopAlgo TKPrim TKBO TKFillet
)
