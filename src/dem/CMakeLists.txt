# Propriedades físicas e parâmetros do modelo MED, unidades SI (E5.7.4a). Sem Qt.
add_library(cad_dem STATIC
    DemUnits.h
    DemProperties.h DemProperties.cpp
)
target_include_directories(cad_dem PUBLIC ${PROJECT_SOURCE_DIR}/src)
