// Constantes da rede cúbica do modelo DEM-DYNA (E5.7.4b.2), em unidades SI. Sem Qt.
//
// Referência: DEM_HIAGO_PRINCIPAL.m, bloco "FOR BILINEAR LAW" (linhas 123 a 131), reproduzido termo
// a termo, na mesma ordem de cálculo do MATLAB:
//   CAF   = 0,1385·CORRCAF                      (l. 123)  fator de calibração (área de fratura)  [-]
//   POA   = 1 − ν²                              (l. 124)  [-]
//   delta = 9ν/(4 − 8ν)                         (l. 125)  [-]
//   alfa  = (9 + 8·delta)/(18 + 24·delta)       (l. 126)  [-]
//   LCR   = 2·CAF·POA/(alfa·RFC²)               (l. 127)  comprimento crítico                    [m]
//   EPR   = RFC·√(GRF/(YNG·POA))                (l. 128)  deformação de pico de referência       [-]
//   ENR   = YNG·LCO²·alfa                       (l. 129)  rigidez axial E·A das barras normais    [N]
//   ENRD  = YNG·(LCO·√3/2)²·alfa                (l. 130)  [N]
//   EDG   = 2·delta·(4/3)·(ENRD/√3)             (l. 131)  rigidez axial E·A das diagonais         [N]
// com RFC = 1/√d_eq (E5.7.4a.1). Portanto LCR = 2·CAF·POA·d_eq/alfa e EPR = √(GRF/(d_eq·E·POA)),
// exatamente as grandezas de derive() (DemProperties.h); EPR é obtido de derive() para que exista
// uma única implementação da fórmula. Algebricamente, EDG = (2·delta/√3)·ENR.
//
// ENR e EDG são produtos E·A (força por unidade de deformação), não rigidezes de mola: a rigidez
// inicial de uma barra de comprimento Li é E·A/Li (BilinearLaw.h). As áreas usam o lado LCO da rede
// regular, como no MATLAB, também quando a geometria foi projetada (ver BilinearLaw.h).
//
// Nada aqui depende da malha nem do campo aleatório de Gf.
#pragma once

#include "dem/DemProperties.h"

namespace cad::dem {

// Constante de calibração do MATLAB (DEM_HIAGO_PRINCIPAL.m, linha 123: CAF = 0.1385 * CORRCAF).
inline constexpr double kCafBase = 0.1385;

struct LatticeConstants
{
    double lco = 0.0;   // LCO   aresta do módulo cúbico          [m]
    double caf = 0.0;   // CAF                                    [-]
    double rfc = 0.0;   // RFC = 1/√d_eq                          [m^(-1/2)]
    double poa = 0.0;   // POA = 1 − ν²                           [-]
    double delta = 0.0; // delta                                  [-]
    double alpha = 0.0; // alfa                                   [-]
    double lcr = 0.0;   // LCR                                    [m]
    double epr = 0.0;   // EPR                                    [-]
    double enr = 0.0;   // ENR  (E·A das barras normais)          [N]
    double enrd = 0.0;  // ENRD (termo intermediário do MATLAB)   [N]
    double edg = 0.0;   // EDG  (E·A das diagonais)               [N]
};

// Verifica as entradas de computeLattice: propriedades válidas (validate()) e LCO finito e positivo;
// além disso, todas as constantes resultantes finitas e positivas.
PropertyValidation validateLattice(const DemProperties& properties, double lcoMeters);

// Constantes da rede para LCO em metros. Pré-condição: validateLattice(...).ok.
LatticeConstants computeLattice(const DemProperties& properties, double lcoMeters);

} // namespace cad::dem
