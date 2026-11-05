// Tabela de leis constitutivas DEM-DYNA (E5.7.4b.5), transcrição de lawnumbers.m. Sem Qt e sem
// dependência de cad_discretization: as entradas por barra (tp, Li, Gf) chegam como vetores, na
// ordem dos elementos EN (ID do elemento = índice + 1). Na aplicação, tp vem de
// discretization::classifyLattice (LatticeTopology::linkTp) e Li de discretization::linkLengths(...,
// LengthUnit::Meter).
//
// Referência: lawnumbers.m (somente leitura), reproduzido operação por operação:
//   kr(b)  = LCR/Li(b)                                         para TODAS as barras   (l. 19)
//   Deduplicação literal (l. 21-32): para b > 1, procura i = b−1, b−2, ..., 1 e para na PRIMEIRA
//   barra encontrada (a mais próxima) com |Gf(b) − Gf(i)| < LCO·1e−6 (desigualdade estrita,
//   tolerância absoluta, LCO em metros) E tp(b) == tp(i). A barra b copia EEP, FP, EEU e a lei de i.
//   A tolerância não é transitiva: a barra mais próxima pode pertencer a uma lei diferente de
//   outra barra que também satisfaria a tolerância; vale a mais próxima, como no MATLAB.
//   Lei nova (l. 34-38): k = k + 1; mat_bonds(b) = k; lawBond(k) = b (primeira barra da lei).
//   Valores da lei nova, com Gfb(k) = Gf(b) (l. 41-64):
//     EEP = EPR·Gfb(k)                     (tp = 1, 2, 4, 0)
//     FP  = (ENR/4)·EEP    tp = 1
//           (ENR/2)·EEP    tp = 2
//           ENR·EEP        tp = 4
//           EDG·EEP        tp = 0
//     EEU = EEP·kr(b)
//   tp = 3: lawnumbers.m não tem ramo; EEP = FP = EEU = 0 (vetores iniciados com zeros). A lei é
//   criada, numerada e associada normalmente. A extensão 3·ENR/4 de BilinearLaw.h NÃO é usada.
//   Deslocamento das curvas de descarga (l. 72): jj = round(k, −1) + 10 (arredondamento do MATLAB,
//   metade para longe de zero: k = 5 → 20, 45 → 60).
//   Identificadores (l. 73-108): lei j → MAT j, TB,DISCRETE j, curva de carga j, curva de descarga
//   j + jj. A curva da lei usa Li e kr da PRIMEIRA barra (lawBond(k)), mesmo que outras barras da
//   mesma lei tenham comprimentos diferentes (Li não participa da deduplicação).
//
// Nenhuma validação física é feita aqui (leis nulas, kr ≤ 1, E·A ≤ 0 são preservadas como no MATLAB);
// a validação para exportação é etapa posterior (dem/LawDiagnostics.h). Só entradas estruturalmente
// impossíveis no fluxo do MATLAB são recusadas: vetores de tamanhos diferentes, tp fora de 0..4,
// Li não finito ou não positivo, Gf não finito, constantes da rede não finitas ou LCO ≤ 0.
#pragma once

#include "dem/BilinearLaw.h"
#include "dem/LatticeConstants.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cad::dem {

// Uma lei k de lawnumbers.m.
struct MatlabLaw
{
    std::int32_t id = 0;          // k = MAT = TB,DISCRETE = curva de carga
    int tp = -1;                  // 0..4
    double gf = 0.0;              // Gfb(k) = Gf da primeira barra
    std::int64_t firstBond = 0;   // lawBond(k), ID do elemento (base 1)
    double li = 0.0;              // Li da primeira barra [m]
    double kr = 0.0;              // kr da primeira barra [-]
    double eep = 0.0;             // EEP [-]
    double fp = 0.0;              // FP  [N]
    double eeu = 0.0;             // EEU [-]
    bool hasMatlabBranch = false; // false somente para tp = 3 (EEP = FP = EEU = 0)
    RegularizedCurve loading;     // pontos de lawnumbers.m l. 88-89 (matlabLoadingCurve)
    RegularizedCurve unloading;   // pontos de lawnumbers.m l. 96-97 (matlabUnloadingCurve)
    std::int32_t loadingCurveId = 0;   // j
    std::int32_t unloadingCurveId = 0; // j + jj
    // Associação e informação para diagnóstico posterior (comprimentos diferentes na mesma lei).
    std::int64_t assignedBonds = 0;           // barras com mat_bonds = k
    double minLength = 0.0, maxLength = 0.0;  // menor e maior Li entre essas barras [m]
    std::int64_t bondsWithOtherLength = 0;    // barras da lei com Li != li (comparação exata)
    // Maior |Li − li|/li entre as barras da lei.
    double maxRelativeLengthDeviation() const;
};

struct MatlabLawTable
{
    bool ok = false;
    std::string message;
    double lco = 0.0;       // LCO [m]
    double tolerance = 0.0; // LCO·1e−6 (tolerância absoluta em Gf)
    std::int32_t curveOffset = 0; // jj
    std::vector<MatlabLaw> laws;  // índice = id − 1
    // Saídas por barra de lawnumbers.m (índice = ID do elemento − 1).
    std::vector<std::int32_t> bondLaw; // mat_bonds
    std::vector<double> bondKr;        // kr
    std::vector<double> bondEep;       // EEP
    std::vector<double> bondFp;        // FP
    std::vector<double> bondEeu;       // EEU
    // Estatística da busca literal (número de comparações |ΔGf| / tp avaliadas).
    std::int64_t comparisons = 0;

    std::int64_t bonds() const { return std::int64_t(bondLaw.size()); }
    std::int32_t lawCount() const { return std::int32_t(laws.size()); }
    // Leis cujas barras não têm todas o mesmo comprimento da primeira barra (IDs das leis).
    std::vector<std::int32_t> lawsWithMixedLengths() const;
};

// jj = round(k, −1) + 10 do MATLAB para k ≥ 0 leis.
std::int32_t matlabCurveOffset(std::int64_t lawCount);

// Tabela de leis de lawnumbers.m. 'tp', 'lengthMeters' e 'gf' por barra, na ordem dos elementos.
// Constantes usadas: lattice.lco, lcr, epr, enr, edg.
MatlabLawTable buildMatlabLawTable(const LatticeConstants& lattice, const std::vector<std::int8_t>& tp,
                                   const std::vector<double>& lengthMeters, const std::vector<double>& gf);

// Classes do campo de Weibull de GF_field.m (l. 38-67), sem gerador aleatório. Mesmas expressões e
// ordem de cálculo do MATLAB:
//   GM = 35, err = 1, DG = 0,01; enquanto err ≠ 0: err > 0 → GM −= DG; err < 0 → GM += DG, DG /= 2;
//       DE = Cv² + 1 − Γ(1 + 2/GM)/Γ(1 + 1/GM)²;  err = fix(10000·DE)
//   BT = Γ(1 + 1/GM)
//   x(i)  = ((ln(1/((inter − i)/inter)))^(1/GM)/BT)^0,5,          i = 1..inter−1
//   xm(1) = (x(1) + ((ln(1/0,999))^(1/GM)/BT)^0,5)/2
//   xm(i) = (x(i) + x(i−1))/2,                                       i = 2..inter−1
//   xm(inter) = (x(inter−1) + (ln(1/0,001)^(1/GM)/BT)^0,5)/2
// Gf_bond de cada barra é sempre um dos valores xm (GF_field.m l. 215-227). Com CVgf = 0 o MATLAB
// não usa as classes (Gf_bond = 1, l. 278).
struct WeibullClasses
{
    bool ok = false;
    std::string message;
    double cv = 0.0;
    int inter = 0;
    double gm = 0.0;        // GM
    double bt = 0.0;        // BT
    std::vector<double> x;  // x(1..inter−1)   (índice = i − 1)
    std::vector<double> xm; // xm(1..inter)    (índice = i − 1)
};

// Recusa: cv não finito ou ≤ 0, inter < 2, iteração de GM sem convergência.
WeibullClasses weibullBins(double cv, int inter);

} // namespace cad::dem
