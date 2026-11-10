// Composição em memória do arquivo APDL/LS-DYNA do modelo DEM-DYNA (E5.7.4b.8b). Sem Qt e sem
// escrita em disco: o resultado é um texto (ou nenhum texto, com os diagnósticos) que a gravação
// segura da etapa seguinte (E5.7.4b.8c) recebe.
//
// API pública: uma única função de composição, composeDemDyna(const DemDynaInput&). A entrada só
// contém dados primários (discretização, propriedades, Gf, exclusões, cabeçalho); nós, barras, tp,
// comprimentos, massas, constantes da rede e leis são SEMPRE reconstruídos internamente a partir da
// discretização e das propriedades. Não existe caminho público que aceite um modelo intermediário
// pronto: a montagem e a composição do modelo são detalhes internos (DemDynaApdl.cpp).
//
// Reutiliza, sem recalcular: Discretization (nós e numeração), ApdlLinkSequence e linkLengths
// (barras EN e Li), LatticeTopology (tp e massas nodais, matlabMassBlock), LatticeConstants
// (constantes da rede), MatlabLawTable (leis de lawnumbers.m), MatlabLawApdl (texto das leis e do
// EMODIF) e LawValidation (bloqueio). Nenhuma fórmula do MATLAB é reescrita aqui.
//
// Ordem dos comandos (DEM_HIAGO_PRINCIPAL.m, malha_por_mascara.m, GF_field.m, lawnumbers.m):
//   [!seed == <seed %g>]                 somente se fornecido (GF_field.m)
//   Finish
//   /clear
//   [!Code generate on <texto>]          somente se fornecido (nenhuma data é gerada aqui)
//   [/CWD,'<diretório>']                 somente se fornecido (opcional, nunca obrigatório)
//   /title, DEM-DYNA
//   /PREP7
//   n, <id>,<x %g>,<y %g>,<z %g>         um por nó, metros, IDs 1..N (vértices e depois centros)
//   ET,1,165
//   EN, <id>,<I>,<J>                     uma por barra, na ordem de ApdlLinkSequence
//   <bloco de massas>                    matlabMassBlock (NODAL DENSITY, ET,2,166, R/TYPE/REAL/E)
//   [<comentário CAMPO Gf>]              somente se fornecido (GF_field.m, CVgf > 0)
//   <leis>                               matlabLawBlock (*DIM, EDCURVE, MAT, TB,DISCRETE, TBDAT)
//   <associações>                        matlabEmodifBlock (DEFINE A LEI..., EMODIF)
//   ESEL,S,TYPE,,1
//   EPLOT
//
// Formatação:
//   - coordenadas: "%g" do MATLAB (matlabG), em metros: modo de equivalência com malha_por_mascara.m;
//   - IDs de nós e elementos em "n" e "EN": inteiros decimais exatos. CORREÇÃO DE SEGURANÇA: o
//     MATLAB escreve I e J de "EN" com "%g" (malha_por_mascara.m, "EN, %u,%g,%g"), que só é exato até
//     999999; a partir de 10^6 o "%g" arredonda a 6 algarismos (1234567 → "1.23457e+06" = 1234570) e
//     liga a barra a outro nó. Para IDs < 10^6 o texto é idêntico byte a byte ao do MATLAB;
//   - blocos constitutivos (leis, EMODIF) e de massas: textos aprovados de MatlabLawApdl e
//     LatticeTopology, sem alteração. Os IDs de lei e de curva desses blocos usam "%g"; a composição
//     é recusada se lei + jj ≥ 10^6 (inteiro não representável exatamente em "%g").
//
// Precisão das coordenadas escritas (checkApdlCoordinates): o "%g" (6 algarismos significativos) é
// mantido. Antes de aceitar a composição, cada coordenada é formatada exatamente como na linha "n" e
// relida (strtod); a geometria escrita é comparada com a original por igualdade exata, sem
// tolerância:
//   - valor escrito não finito                                   → erro;
//   - nós com coordenadas originais diferentes e coordenadas escritas iguais (nas três direções)
//     → colapso introduzido pela formatação: erro;
//   - barra com extremidades originais diferentes e extremidades escritas iguais → barra degenerada
//     no APDL: erro.
// Nós já coincidentes na geometria original não são tratados como colapso de formatação (decisão
// geométrica separada; a política existente não os verifica). O arredondamento do "%g" é monótono
// (x < y ⇒ g(x) ≤ g(y)), portanto não inverte a ordem das coordenadas: a única perda possível é a
// igualdade de valores antes distintos, que é exatamente o que se verifica. Nenhuma coordenada é
// corrigida.
//
// Exclusões (EDELE): o vetor 'deleted' participa da validação das leis (InvalidEmitted), mas o
// bloco EDELE NÃO é escrito: não há comportamento de referência validado (buildcrack.m não é
// transcrito e o DEM_DYNA.txt não tem trinca). Com qualquer barra excluída a composição é recusada.
//
// Validação antes da composição (nada é composto se houver erro):
//   1. entradas: discretização não vazia, propriedades e LCO (validateLattice), Gf obrigatório;
//   2. rede: classifyLattice, constantes finitas;
//   3. nós (coordenadas finitas) e conectividade (I, J em 1..N, I ≠ J);
//   4. comprimentos (finitos, > 0) e massas (uma por nó, finitas, > 0, LCO coerente);
//   5. tamanhos dos vetores por barra (tp, Li, Gf, deleted) e cabeçalho sem quebras de linha;
//   6. representação APDL das coordenadas (checkApdlCoordinates);
//   7. tabela de leis (buildMatlabLawTable) e validateLawTable: InvalidExported e InvalidEmitted
//      bloqueiam; nenhuma lei é corrigida ou substituída.
// As entradas nunca são alteradas.
#pragma once

#include "dem/DemProperties.h"
#include "dem/LawDiagnostics.h"
#include "discretization/Discretization.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cad::discretization {

// Linhas do cabeçalho que dependem de dados externos. Ausente = linha não escrita.
struct DemDynaHeader
{
    std::optional<double> seed;                  // "!seed == %g"
    std::optional<std::string> timestamp;        // "!Code generate on <texto>", texto literal
    std::optional<std::string> workingDirectory; // "/CWD,'<diretório>'"
};

// Comentário escrito por GF_field.m quando CVgf > 0 (somente descritivo; o campo não é gerado aqui):
//   (linha vazia)
//   !************************CAMPO Gf ***********
//   ! Rotação: X=<%.1f>°, Y=<%.1f>°, Z=<%.1f>°      (somente se alguma rotação ≠ 0; UTF-8)
struct DemDynaGfFieldNote
{
    std::array<double, 3> rotationDegrees{}; // tetax, tetay, tetaz
};

// Dados primários da composição. Nada derivado (nós, barras, tp, Li, massas, leis) é aceito de fora.
struct DemDynaInput
{
    const Discretization* discretization = nullptr; // obrigatória, não vazia
    dem::DemProperties properties;                  // ROH, PSN, YNG, GRF, d_eq
    std::vector<double> gf;                         // Gf_bond por barra (ordem EN), obrigatório
    std::vector<bool> deleted;                      // barras excluídas; vazio = nenhuma
    DemDynaHeader header;
    std::optional<DemDynaGfFieldNote> gfField;
};

// Erro de entrada, de consistência ou de representação (fora da tabela de leis).
struct DemDynaIssue
{
    std::string stage;   // "entrada", "propriedades", "rede", "nós", "conectividade", "comprimentos",
                         // "massas", "Gf", "exclusões", "cabeçalho", "representação APDL", "leis"
    std::int64_t id = 0; // primeiro nó/elemento afetado; 0 = não se aplica
    std::string message;
};

struct DemDynaComposition
{
    bool ok = false;                 // true somente se 'text' é utilizável
    std::string text;                // vazio quando ok = false
    std::vector<DemDynaIssue> errors;
    bool lawsValidated = false;      // validateLawTable executada
    dem::LawValidationReport lawReport;
    std::int64_t nodes = 0, links = 0, masses = 0;
    std::int32_t laws = 0, curveOffset = 0;
};

// Valida as entradas, reconstrói nós, barras, tp, comprimentos, massas, constantes e leis, executa
// validateLawTable e compõe o texto. Com qualquer erro: ok = false e text vazio.
DemDynaComposition composeDemDyna(const DemDynaInput& input);

// Verificação da representação APDL das coordenadas (critérios acima). Somente diagnóstico: não
// produz texto APDL nem altera os dados. 'nodesMeters' por ID − 1; 'links' com IDs (I, J) de nós
// (barras com IDs fora de 1..N são ignoradas aqui; a conectividade é verificada na composição).
struct DemDynaCoordinateReport
{
    std::int64_t nonFinite = 0;      // nós com coordenada escrita não finita
    std::int64_t collapsedNodes = 0; // nós que colapsam sobre outro nó originalmente distinto
    std::int64_t degenerateBars = 0; // barras com extremidades escritas iguais (originais distintas)
    std::int64_t coincidentOriginal = 0; // nós originalmente coincidentes com outro (não é erro aqui)
    std::vector<DemDynaIssue> issues;    // detalhes (limitados) com IDs, coordenadas originais e escritas
    bool ok() const { return nonFinite == 0 && collapsedNodes == 0 && degenerateBars == 0; }
};
DemDynaCoordinateReport checkApdlCoordinates(const std::vector<std::array<double, 3>>& nodesMeters,
                                             const std::vector<std::array<std::int64_t, 2>>& links);

// Linhas usadas na composição (formatação somente; expostas para teste).
std::string demDynaHeaderText(const DemDynaHeader& header);
std::string demDynaNodeLine(std::int64_t id, const std::array<double, 3>& xyzMeters); // "n, <id>,<x>,<y>,<z>\n"
std::string demDynaElementLine(std::int64_t id, std::int64_t i, std::int64_t j);    // "EN, <id>,<I>,<J>\n"
std::string demDynaGfFieldText(const DemDynaGfFieldNote& note);

} // namespace cad::discretization
