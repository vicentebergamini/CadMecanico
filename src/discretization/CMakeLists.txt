# Discretização volumétrica em cubos elementares, exportação ASCII dos nós (E5.7.2) e exportação
# geométrica ANSYS APDL /PREP7 (E5.7.2.2), projeção dos nós sobre o contorno (E5.7.2.3) e classificação
# tp das barras e massas nodais da rede DEM-DYNA (E5.7.4b.3) e texto APDL das leis constitutivas
# (E5.7.4b.6), sem Qt. Depende de cad_dem (tabela de leis); cad_dem não depende desta biblioteca.
add_library(cad_discretization STATIC
    VoxelGrid.h
    Discretization.h Discretization.cpp
    CubeClassifier.h CubeClassifier.cpp
    VoxelDiscretizer.h VoxelDiscretizer.cpp
    NodeAsciiExporter.h NodeAsciiExporter.cpp
    ApdlExporter.h ApdlExporter.cpp
    BoundaryProjector.h BoundaryProjector.cpp
    LatticeTopology.h LatticeTopology.cpp
    MatlabLawApdl.h MatlabLawApdl.cpp
)
target_include_directories(cad_discretization PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_discretization PUBLIC
    TKernel TKMath TKG3d TKGeomBase TKGeomAlgo TKBRep TKTopAlgo TKPrim
    cad_dem
)
