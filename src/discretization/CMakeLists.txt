# Discretização volumétrica em cubos elementares, exportação ASCII dos nós (E5.7.2) e exportação
# geométrica ANSYS APDL /PREP7 (E5.7.2.2), sem Qt.
add_library(cad_discretization STATIC
    VoxelGrid.h
    Discretization.h Discretization.cpp
    CubeClassifier.h CubeClassifier.cpp
    VoxelDiscretizer.h VoxelDiscretizer.cpp
    NodeAsciiExporter.h NodeAsciiExporter.cpp
    ApdlExporter.h ApdlExporter.cpp
)
target_include_directories(cad_discretization PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_discretization PUBLIC
    TKernel TKMath TKG3d TKGeomBase TKBRep TKTopAlgo TKPrim
)
