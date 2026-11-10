// Validação da tabela de leis DEM-DYNA antes da exportação APDL/DYNA (E5.7.4b.7). Sem Qt.
//
// Somente leitura: a tabela (dem::MatlabLawTable, transcrição de lawnumbers.m) não é alterada,
// nenhum valor é corrigido e nenhuma lei é removida ou substituída. A validação só produz
// diagnósticos (dem/LawDiagnostics.h); a decisão de exportar fica com quem chama (E5.7.4b.8).
//
// Entradas:
//   table    tabela de buildMatlabLawTable (ordem dos elementos EN; ID do elemento = índice + 1);
//   deleted  barras excluídas (futuro EDELE), uma entrada por barra; vazio = nenhuma excluída.
//            Barra ativa = barra não excluída.
//
// Os comandos de TODAS as leis e o EMODIF de TODAS as barras são escritos, como em lawnumbers.m
// (DEM_HIAGO_PRINCIPAL.m chama lawnumbers antes do bloco EDELE). Por isso uma lei inválida usada só
// por barras excluídas ainda escreve EDCURVE/TB,DISCRETE inválidos: diagnóstico InvalidEmitted
// (bloqueante). Ver a decisão pendente no relatório da E5.7.4b.7.
//
// Critérios (categoria; tipo conforme a lei tenha barra ativa → InvalidExported, ou não → InvalidEmitted):
//   Estruturais
//     S1 tabela inválida (ok = false); vetores por barra (mat_bonds, kr, EEP, FP, EEU, deleted) de
//        tamanhos diferentes;
//     S2 IDs das leis 1..k em ordem; jj = round(k, −1) + 10; curva de carga = j, de descarga = j + jj;
//        todos os IDs de curva positivos e distintos;
//     S3 mat_bonds de cada barra em 1..k (sem referência a lei inexistente);
//     S4 primeira barra da lei (lawBond) válida, atribuída à lei e primeira ocorrência dela;
//        número de barras atribuídas igual ao registrado; nenhuma lei sem barra;
//     S5 EEP, FP e EEU de cada barra iguais aos da sua lei (cópia literal de lawnumbers.m l. 26-29);
//        kr da lei igual ao kr da primeira barra; tp em 0..4; ramo MATLAB ausente só para tp = 3;
//        tp = 3 com EEP = FP = EEU = 0.
//   Numéricos
//     N1 EEP, FP, EEU, kr e Li da lei finitos; kr de cada barra finito;
//     N2 pontos (abscissas e forças) das curvas de carga e descarga finitos;
//     N3 abscissas estritamente crescentes nos valores ESCRITOS (%g, 6 algarismos significativos),
//        quando já o são em precisão dupla.
//   Constitutivos
//     C1 tp ≠ 3: EEP, FP e EEU estritamente positivos;
//     C2 tp = 3: lawnumbers.m não tem ramo (EEP = FP = EEU = 0; 3·ENR/4 não aplicado):
//        com barra ativa → InvalidExported; só barras excluídas → ReferenceZero (informativo);
//     C3 abscissas (−δp, 0, δp, δr, c·δr) estritamente crescentes nas duas curvas (kr > 1).
//        O segundo ponto é o literal "0" escrito por lawnumbers.m.
//   Informativos
//     I1 barras de comprimentos diferentes na mesma lei (a curva usa Li da primeira barra; a
//        associação MATLAB é preservada).
#pragma once

#include "dem/LawDiagnostics.h"
#include "dem/MatlabLawTable.h"

#include <string>
#include <vector>

namespace cad::dem {

LawValidationReport validateLawTable(const MatlabLawTable& table, const std::vector<bool>& deleted);

// Texto de um diagnóstico, com severidade, bloqueio, categoria, tipo, escopo (tabela, barra ou lei e
// bloco APDL), lei, tp, elemento afetado e seu estado, elemento que criou a lei (quando diferente),
// parâmetro, valor e motivo. Ex.:
//   "ERRO, bloqueia a exportação [numérico, InvalidExported; escopo: lei e bloco APDL] lei 1, tp = 4,
//    elemento 2 (ativo), afetado pela lei criada pelo elemento 1 (excluído); 1 barra ativa na lei:
//    valor armazenado na lei kr = Inf. ..."
// Os valores de escopo lei (Li, kr, EEP, FP, EEU, curvas) são os armazenados na lei, calculados a partir
// do elemento de origem (lawBond), nunca o valor geométrico do elemento afetado.
std::string describe(const LawIssue& issue);

// Valor formatado como no diagnóstico ("%.17g"; "Inf", "-Inf", "NaN").
std::string formatIssueValue(double value);

} // namespace cad::dem
