# Esboços 2D (sem Qt): planos, entidades, dimensões e arestas OCCT.
add_library(cad_sketch STATIC
    SketchPlane.h SketchPlane.cpp
    SketchEntity.h SketchEntity.cpp
    Sketch.h Sketch.cpp
    SketchShapes.h SketchShapes.cpp
    SketchProfile.h SketchProfile.cpp
    SketchResolver.h SketchResolver.cpp
)
target_include_directories(cad_sketch PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_sketch PUBLIC TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKTopAlgo)
