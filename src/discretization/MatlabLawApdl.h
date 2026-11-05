// Texto APDL/DYNA das leis constitutivas e da associação elemento–material (E5.7.4b.6), exatamente
// como lawnumbers.m o escreve. Sem Qt.
//
// Arquitetura: a tabela de leis (dem::MatlabLawTable, E5.7.4b.5) fica em cad_dem, que não depende de
// cad_discretization. O texto é gerado aqui, ao lado do formatador matlabG e do bloco de massas
// (LatticeTopology.h), para que exista um único formatador "%g". Dependência de mão única:
// cad_discretization → cad_dem. Este módulo só formata: nenhum valor, lei ou curva é recalculado,
// validado ou corrigido (os pontos das curvas são os de matlabLoadingCurve/matlabUnloadingCurve
// guardados na tabela).
//
// Formato de lawnumbers.m (l. 71-117), com "\n" como fim de linha e %g/%f do fprintf do MATLAB. Para
// cada lei j = 1..k (ordem de primeira ocorrência), com FP_, EEP_, kr_ e LI_ da primeira barra:
//   (linha vazia)
//   !************************INICIO DA LEI <j>
//   *DIM,f1<j>,,5
//   *DIM,d1<j>,,5
//   f1<j>(1)= <−FP>,0,<FP>,<FP/100>,<FP/100>                            curva de carga, força
//   d1<j>(1)= <−EEP·LI>,0,<EEP·LI>,<EEP·kr·LI>,<2·(EEP·kr·LI)>          curva de carga, deslocamento
//   EDCURVE,ADD,<j>,d1<j>(1),f1<j>(1)
//   *DIM,f2<j>,,5
//   *DIM,d2<j>,,5
//   f2<j>(1)= <−FP>,0,<FP>,<FP/100>,<FP/100>                            curva de descarga, força
//   d2<j>(1)= <−LI·EEP>,0,<LI·EEP>,<LI·kr·EEP>,<100·LI·kr·EEP>           curva de descarga, deslocamento
//   EDCURVE,ADD,<j+jj>,d2<j>(1),f2<j>(1)
//   MAT,<j>
//   TB,DISCRETE,<j>,,,5
//   TBDAT,1,<j>    !CURVA DE CARGA ␠
//   TBDAT,2,<j+jj> !CURVA DESCARGA ␠
//   TBDAT,3,1    !BETA ␠
//   TBDAT,4,<FP> !TYI ␠
//   TBDAT,5,<−FP> !CYI ␠
//   !************************FIM DA LEI <j>
// (␠ = espaço final presente no MATLAB). O segundo ponto de cada curva é o literal "0" do formato do
// MATLAB, não um valor formatado. Todos os números, inclusive os IDs inteiros, usam %g ("-0" para
// zero negativo; notação científica com expoente de dois dígitos, ex. "8.8759e-06").
// Associação (l. 114-117), uma linha por barra b = 1..nBonds na ordem dos elementos EN:
//   ! ***********************DEFINE A LEI EM CADA MOLA e ENERGIAS
//   EMODIF,<b %f>,MAT,<lei %f>, ␠            ex.: "EMODIF,1.000000,MAT,1.000000, "
// Diferença conhecida do fprintf do MATLAB: valores não finitos são escritos "Inf"/"NaN" pelo MATLAB
// e "inf"/"nan" pelo C; a tabela não produz valores não finitos a partir de entradas válidas, e a
// validação para exportação (E5.7.4b.7) deve recusá-los antes da escrita.
#pragma once

#include "dem/MatlabLawTable.h"

#include <cstdint>
#include <string>

namespace cad::discretization {

// Formatação "%f" do fprintf do MATLAB (6 casas decimais), usada em EMODIF.
std::string matlabF(double value);

// Texto de uma lei (do "\n" inicial até "FIM DA LEI <j>\n"), com o jj da tabela.
std::string matlabLawText(const dem::MatlabLaw& law, std::int32_t curveOffset);

// Todas as leis da tabela, na ordem dos IDs. Tabela inválida (ok = false): texto vazio.
std::string matlabLawBlock(const dem::MatlabLawTable& table);

// Cabeçalho "DEFINE A LEI" e um EMODIF por barra. Tabela inválida: texto vazio.
std::string matlabEmodifBlock(const dem::MatlabLawTable& table);

} // namespace cad::discretization
