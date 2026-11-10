# Propriedades físicas e parâmetros do modelo MED, unidades SI (E5.7.4a); constantes da rede e lei
# bilinear DEM-DYNA (E5.7.4b.2); tabela de leis de lawnumbers.m (E5.7.4b.5); validação das leis
# antes da exportação (E5.7.4b.7). Sem Qt.
add_library(cad_dem STATIC
    DemUnits.h
    DemProperties.h DemProperties.cpp
    LatticeConstants.h LatticeConstants.cpp
    BilinearLaw.h BilinearLaw.cpp
    LawDiagnostics.h
    MatlabLawTable.h MatlabLawTable.cpp
    LawValidation.h LawValidation.cpp
)
target_include_directories(cad_dem PUBLIC ${PROJECT_SOURCE_DIR}/src)
