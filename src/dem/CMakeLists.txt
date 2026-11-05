# Propriedades físicas e parâmetros do modelo MED, unidades SI (E5.7.4a); constantes da rede e lei
# bilinear DEM-DYNA (E5.7.4b.2). Sem Qt.
add_library(cad_dem STATIC
    DemUnits.h
    DemProperties.h DemProperties.cpp
    LatticeConstants.h LatticeConstants.cpp
    BilinearLaw.h BilinearLaw.cpp
)
target_include_directories(cad_dem PUBLIC ${PROJECT_SOURCE_DIR}/src)
