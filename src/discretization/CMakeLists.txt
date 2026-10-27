# Discretização volumétrica em cubos elementares, exportação ASCII dos nós (E5.7.2) e exportação
# geométrica ANSYS APDL /PREP7 (E5.7.2.2) e projeção dos nós sobre o contorno (E5.7.2.3), sem Qt.
add_library(cad_discretization STATIC
    VoxelGrid.h
    Discretization.h Discretization.cpp
    CubeClassifier.h CubeClassifier.cpp
    VoxelDiscretizer.h VoxelDiscretizer.cpp
    NodeAsciiExporter.h NodeAsciiExporter.cpp
    ApdlExporter.h ApdlExporter.cpp
    BoundaryProjector.h BoundaryProjector.cpp
)
target_include_directories(cad_discretization PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_discretization PUBLIC
    TKernel TKMath TKG3d TKGeomBase TKGeomAlgo TKBRep TKTopAlgo TKPrim
)
