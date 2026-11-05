// Lei constitutiva bilinear DEM-DYNA de uma barra (E5.7.4b.2), em unidades SI. Sem Qt.
//
// Referências: lawnumbers.m (EEP, FP, EEU, kr e curvas de carga/descarga, linhas 19 a 98) e
// malha_por_mascara.m (tipo tp das barras, linhas 84 a 187). Formulação: lei de Hillerborg da
// Figura 2b (DEM-DYNA), com EPR da E5.7.4a.1 (POA = 1 − ν² mantido).
//
// Grandezas de uma barra de comprimento Li, fator de energia g e rigidez axial E·A:
//   εp = EEP = EPR·g                       deformação crítica              (lawnumbers.m, l. 42/49/55/61)
//   FP = Pcr = E·A·εp                      força crítica, ponto A          (l. 43/50/56/62)
//   kr = LCR/Li                            fator de ruptura                (l. 19)
//   εr = EEU = εp·kr                       deformação de ruptura           (l. 44/51/57/63)
//   k0 = E·A/Li                            rigidez inicial (inclinação O–A da curva, l. 88-89)
//   δp = εp·Li,  δr = εr·Li = EPR·g·LCR    deslocamentos crítico e de ruptura
// g é o fator Gf_bond de GF_field.m: a raiz quadrada da razão Gf_local/Gf_médio (o campo de Weibull
// é gerado com expoente 0,5, GF_field.m l. 59-67 e 118); g = 1 quando CVgf = 0 (l. 278).
//
// Comprimento real Li: todas as grandezas que dependem do comprimento (kr, εr, k0, δp, δr) usam o
// comprimento da própria barra, como UNDEFORMEDLENGTH no MATLAB (malha_por_mascara.m l. 237-246).
// Consequências (válidas também para a geometria projetada da E5.7.2.3, que altera Li):
//   - εp e FP não dependem de Li; δr = EPR·g·LCR também não;
//   - k0 = E·A/Li e δp = εp·Li dependem de Li;
//   - a energia de fratura ideal ½·FP·δr = ½·E·A·EPR²·g²·LCR não depende de Li;
//   - a lei só existe com kr > 1, isto é, Li < LCR (sem "snap-back"); caso contrário o ramo
//     descendente teria abscissas decrescentes e as curvas do LS-DYNA seriam inválidas.
// E·A continua calculado com LCO (ENR, EDG), como no MATLAB, que só trata a rede regular. A
// distribuição de rigidez numa rede distorcida não é definida pela referência (ver relatório).
//
// Duas representações separadas:
//   1. Lei ideal de Hillerborg (Figura 2b): O–A elástico, A–B linear descendente, força NULA a
//      partir de δr; dano secante D = 1 − F/(k0·δmax), irreversível. Definida para δ ≥ 0 (tração).
//   2. Curva regularizada do MATLAB/APDL (lawnumbers.m l. 88-98): 5 pontos; o ramo descendente
//      termina em FP/100 (não em zero) e esse valor é mantido até 2·δr (curva de carga) ou 100·δr
//      (curva de descarga). A força residual NÃO é ruptura completa: ela acrescenta energia
//      (ver regularizedExcessToRupture e regularizedPlateauWork).
// O comportamento do LS-DYNA (*MAT_SPRING_GENERAL_NONLINEAR, TB,DISCRETE,,,,5 com BETA = 1) fora
// dos pontos fornecidos, na descarga e na recarga NÃO é assumido aqui: a validação no ANSYS está
// pendente. Por isso as funções da curva regularizada só respondem dentro do intervalo definido.
#pragma once

#include "dem/LatticeConstants.h"

#include <array>
#include <string>

namespace cad::dem {

// Tipo da barra na rede cúbica. Correspondência com tp de malha_por_mascara.m / lawnumbers.m:
//   EdgeOneCell     tp = 1  aresta com 1 cubo ativo em volta   E·A = ENR/4  (lawnumbers.m l. 41-45)
//   EdgeTwoCells    tp = 2  aresta com 2 cubos ativos          E·A = ENR/2  (l. 48-52)
//   EdgeThreeCells  tp = 3  aresta com 3 cubos ativos          E·A = 3·ENR/4  EXTENSÃO (ver abaixo)
//   EdgeFourCells   tp = 4  aresta com 4 cubos ativos          E·A = ENR    (l. 54-58)
//   CenterCenter    tp = 4  centro a centro (cubos vizinhos)   E·A = ENR    (l. 54-58)
//   Diagonal        tp = 0  centro do cubo a um vértice        E·A = EDG    (l. 60-64)
// EXTENSÃO tp = 3: malha_por_mascara.m produz tp = 3 em arestas de cantos reentrantes (seções
// viga_I, tubo, retangular_vazado), mas lawnumbers.m não tem ramo para tp = 3 e deixa EEP = FP =
// EEU = 0 (mola sem resistência). A regra 3·ENR/4 segue a proporção tp/4 declarada em
// malha_por_mascara.m (l. 14) e NÃO faz parte da referência executável: isMatlabReference() = false.
enum class BarType
{
    EdgeOneCell,
    EdgeTwoCells,
    EdgeThreeCells,
    EdgeFourCells,
    CenterCenter,
    Diagonal
};

// Código tp do MATLAB (1, 2, 3, 4, 4, 0).
int matlabTp(BarType type);
// true se o tipo é tratado por lawnumbers.m; false somente para EdgeThreeCells (extensão).
bool isMatlabReference(BarType type);
const char* barTypeLabel(BarType type);
// E·A da barra [N] (ENR·tp/4, ENR ou EDG).
double axialRigidity(const LatticeConstants& lattice, BarType type);

struct BilinearLaw
{
    BarType type = BarType::EdgeFourCells;
    double length = 0.0;         // Li                   [m]
    double gfFactor = 0.0;       // g (Gf_bond)          [-]
    double axialRigidity = 0.0;  // E·A                  [N]
    double stiffness = 0.0;      // k0 = E·A/Li          [N/m]
    double strainPeak = 0.0;     // εp = EEP             [-]
    double forcePeak = 0.0;      // FP = Pcr             [N]
    double kr = 0.0;             // kr = LCR/Li          [-]
    double strainRupture = 0.0;  // εr = EEU             [-]
    double dispPeak = 0.0;       // δp = εp·Li           [m]
    double dispRupture = 0.0;    // δr = εr·Li           [m]
};

struct BilinearLawResult
{
    bool ok = false;
    std::string message;
    BilinearLaw law;
};

// Lei da barra. Recusa: Li ou g não finitos ou não positivos; E·A não positivo (diagonais com
// ν ≤ 0, em que EDG ≤ 0); kr ≤ 1 (Li ≥ LCR); grandezas não representáveis.
BilinearLawResult makeBilinearLaw(const LatticeConstants& lattice, BarType type, double lengthMeters,
                                  double gfFactor = 1.0);

// ---------------------------------------------------------------- lei ideal de Hillerborg
// Todas definidas para δ ≥ 0; NaN para δ < 0 ou não finito.
double idealForce(const BilinearLaw& law, double displacement);       // envoltória F(δ) [N]
double idealWork(const BilinearLaw& law, double displacement);        // ∫0..δ F dδ (envoltória) [J]
double idealDamage(const BilinearLaw& law, double maxDisplacement);   // D(δmax) ∈ [0, 1]
// Estado com deslocamento máximo δmax, descarga secante até a origem:
double idealElasticEnergy(const BilinearLaw& law, double maxDisplacement);   // triângulo OPC [J]
double idealDissipatedEnergy(const BilinearLaw& law, double maxDisplacement); // área OAP [J]
double idealFractureEnergy(const BilinearLaw& law); // ½·FP·δr: energia total dissipada até B [J]

// ---------------------------------------------------------------- curva regularizada MATLAB/APDL
inline constexpr double kMatlabResidualFraction = 0.01;  // FP/100 (lawnumbers.m l. 88, 96)
inline constexpr double kMatlabLoadingTailFactor = 2.0;   // último ponto da curva de carga: 2·δr (l. 89)
inline constexpr double kMatlabUnloadingTailFactor = 100.0; // último ponto da descarga: 100·δr (l. 97)

struct RegularizedCurve
{
    std::array<double, 5> displacement{}; // [m]
    std::array<double, 5> force{};        // [N]
};

// Pontos exatamente como lawnumbers.m os calcula (mesma ordem das operações):
//   carga    d = (−EEP·LI, 0, EEP·LI, EEP·kr·LI, 2·(EEP·kr·LI)),  f = (−FP, 0, FP, FP/100, FP/100)
//   descarga d = (−LI·EEP, 0, LI·EEP, LI·kr·EEP, 100·LI·kr·EEP),  f = idem
RegularizedCurve matlabLoadingCurve(const BilinearLaw& law);
RegularizedCurve matlabUnloadingCurve(const BilinearLaw& law);

// Interpolação linear por trechos DENTRO do intervalo dos pontos; NaN fora dele (o que o solver faz
// além do último ponto não é assumido).
double curveForce(const RegularizedCurve& curve, double displacement);
// ∫0..δ F dδ ao longo da curva, para 0 ≤ δ ≤ último ponto; NaN fora.
double curveWork(const RegularizedCurve& curve, double displacement);

// Contribuições energéticas da força residual em relação à lei ideal (curva de carga):
//   até δr:       ½·(FP/100)·(δr − δp)   (o ramo A–B termina em FP/100 em vez de 0)
//   de δr a 2·δr: (FP/100)·δr           (patamar residual dentro da curva definida)
double regularizedExcessToRupture(const BilinearLaw& law);
double regularizedPlateauWork(const BilinearLaw& law);

} // namespace cad::dem
