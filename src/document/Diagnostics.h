// Diagnóstico do documento (E5.7 / P2): lista de erros e avisos com a operação afetada, a
// descrição do problema, a causa provável, a orientação para correção e o estado de validade.
// Núcleo sem Qt: a interface (painel "Diagnóstico") apenas exibe a lista.
//
// Erros: operações inválidas após a reconstrução (a primeira falha e as que dependem dela).
// Avisos: situações em que a geometria existe mas requer atenção (esboço vazio ou sem perfil
// fechado, referência recuperada por correspondência aproximada).
#pragma once

#include "document/CadDocument.h"

#include <string>
#include <vector>

namespace cad::document {

enum class DiagnosticSeverity
{
    Error,
    Warning
};

struct Diagnostic
{
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string itemId;      // operação ("op-N") ou esboço ("sk-N"); vazio = documento
    std::string itemName;    // nome exibido da operação/esboço
    std::string description; // o que está errado
    std::string cause;       // causa provável
    std::string guidance;    // como corrigir
    std::string state;       // estado de validade ("Inválida", "Válida com aviso", ...)
};

// Causa provável e orientação para uma mensagem de falha de reconstrução.
struct FailureExplanation
{
    std::string cause;
    std::string guidance;
};
FailureExplanation explainFailure(OperationType type, const std::string& message);

// Diagnóstico completo do documento no estado atual (não altera o documento).
std::vector<Diagnostic> diagnose(const CadDocument& document);

const char* severityLabel(DiagnosticSeverity severity);

} // namespace cad::document
