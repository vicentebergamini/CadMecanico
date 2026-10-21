#include "document/Diagnostics.h"

#include "sketch/SketchProfile.h"

#include <algorithm>

namespace cad::document {

namespace {

bool has(const std::string& text, const char* fragment)
{
    return text.find(fragment) != std::string::npos;
}

std::string capitalized(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = char(s[0] - 'a' + 'A');
    return s;
}

std::string label(const Operation& op)
{
    return op.name + " (" + op.id + ")";
}

} // namespace

const char* severityLabel(DiagnosticSeverity severity)
{
    return severity == DiagnosticSeverity::Error ? "Erro" : "Aviso";
}

FailureExplanation explainFailure(OperationType type, const std::string& message)
{
    // Referências persistentes (faces de suporte e arestas de acabamentos).
    if (has(message, "não remapeável") || has(message, "não identificada") || has(message, "remapear"))
        return {"A geometria anterior mudou e a face ou aresta referenciada não existe mais no corpo "
                "reconstruído (referência perdida).",
                type == OperationType::Extrude
                    ? "Edite o esboço de suporte ou recrie-o sobre a face desejada; ou desfaça a alteração da "
                      "operação anterior."
                    : "Edite a operação e selecione novamente as arestas; ou desfaça a alteração da operação "
                      "anterior."};
    if (has(message, "ambígua"))
        return {"Mais de uma face ou aresta do corpo reconstruído corresponde à referência; a escolha não é "
                "segura e não foi feita automaticamente.",
                "Edite a operação e selecione explicitamente a face ou as arestas desejadas."};
    if (has(message, "inviável") || has(message, "BRepCheck") || has(message, "número de sólidos")
        || has(message, "sem remoção de material"))
        return {type == OperationType::Fillet3D ? "Raio grande demais para a geometria das arestas (faces "
                                                  "adjacentes curtas ou concordâncias que se cruzam)."
                : type == OperationType::Chamfer3D ? "Distância ou ângulo do chanfro incompatível com as faces "
                                                     "adjacentes (recuo maior que a face)."
                                                   : "Valor geométrico incompatível com a geometria atual.",
                "Reduza o valor (raio, distância ou ângulo) ou escolha outras arestas."};
    if (has(message, "não toca") || has(message, "não intersecta") || has(message, "não está no sentido")
        || has(message, "não adiciona material") || has(message, "não há material"))
        return {"A ferramenta da extrusão não alcança o corpo de destino no sentido escolhido.",
                "Verifique o sentido (Inverter sentido), a profundidade, a condição final e o corpo de destino."};
    // Mensagens do reconhecimento do perfil ("perfil do Esboço N inválido: ...", "Perfil aberto",
    // "Contorno ..."); "fora do perfil" (corte externo) não é falha do perfil.
    if (has(message, "perfil do ") || has(message, "Perfil ") || has(message, "contorno") || has(message, "Contorno"))
        return {"O esboço de origem não forma um contorno fechado válido (extremidades livres, "
                "autointerseção ou vários contornos).",
                "Edite o esboço e feche o contorno; depois a operação é recalculada."};
    if (has(message, "Interseção vazia") || has(message, "eliminou todo") || has(message, "sem volume"))
        return {"Os corpos envolvidos não têm volume em comum ou o resultado não tem volume.",
                "Revise os corpos A e B ou o tipo de operação booleana."};
    if (has(message, "inexistente") || has(message, "sem geometria") || has(message, "posterior"))
        return {"A operação referencia um corpo, esboço ou face que não está disponível nesta posição do "
                "histórico.",
                "Edite a operação e escolha uma referência existente e anterior."};
    if (has(message, "Profundidade") || has(message, "profundidade"))
        return {"Profundidade fora dos limites aceitos.", "Informe uma profundidade positiva dentro dos limites."};
    return {"Falha do núcleo geométrico ao construir a operação com os parâmetros atuais.",
            "Edite a operação e altere os parâmetros; se a falha continuar, desfaça a alteração anterior."};
}

std::vector<Diagnostic> diagnose(const CadDocument& document)
{
    std::vector<Diagnostic> list;
    for (const Operation& op : document.operations())
    {
        if (op.status != OperationStatus::Invalid)
            continue;
        Diagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.itemId = op.id;
        d.itemName = op.name;
        if (!op.blockedBy.empty())
        {
            const Operation* blocker = document.findOperation(op.blockedBy);
            const std::string b = blocker ? label(*blocker) : op.blockedBy;
            d.description = "Não calculada: depende de " + b + ", que está inválida.";
            d.cause = "A operação usa o resultado de " + b + ".";
            d.guidance = "Corrija primeiro " + b + "; esta operação será recalculada automaticamente.";
            d.state = "Inválida (bloqueada)";
        }
        else
        {
            const FailureExplanation e = explainFailure(op.type, op.statusMessage);
            d.description = capitalized(op.statusMessage.empty() ? "Falha na reconstrução" : op.statusMessage) + ".";
            d.cause = e.cause;
            d.guidance = e.guidance;
            d.state = "Inválida";
        }
        list.push_back(d);
    }
    for (const SketchIssue& issue : document.sketchIssues())
    {
        const cad::sketch::Sketch* s = document.findSketch(issue.sketchId);
        Diagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.itemId = issue.sketchId;
        d.itemName = s ? s->name() : issue.sketchId;
        d.description = capitalized(issue.message) + ".";
        const FailureExplanation e = explainFailure(OperationType::Extrude, issue.message);
        d.cause = e.cause;
        d.guidance = e.guidance;
        d.state = "Esboço sem suporte (plano mantido do último estado válido)";
        list.push_back(d);
    }
    // Avisos: esboços que não podem ser extrudados (vazios ou sem contorno fechado), quando não
    // usados por nenhuma operação (os usados já aparecem como erro da operação, se falharem).
    for (const cad::sketch::Sketch& s : document.sketches())
    {
        if (!document.dependentOperations(s.id()).empty())
            continue;
        if (std::any_of(document.sketchIssues().begin(), document.sketchIssues().end(),
                        [&](const SketchIssue& i) { return i.sketchId == s.id(); }))
            continue;
        Diagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.itemId = s.id();
        d.itemName = s.name();
        d.state = "Válido com aviso";
        if (s.entities().empty())
        {
            d.description = "Esboço vazio.";
            d.cause = "O esboço não tem entidades.";
            d.guidance = "Edite o esboço e desenhe um contorno, ou exclua-o se não for necessário.";
            list.push_back(d);
            continue;
        }
        const cad::sketch::ProfileResult profile = cad::sketch::buildProfile(s);
        if (!profile.ok)
        {
            d.description = "Esboço sem perfil fechado: não pode ser extrudado.";
            d.cause = profile.message;
            d.guidance = "Feche o contorno (extremidades coincidentes) para extrudar; esboços abertos servem "
                         "apenas como referência.";
            list.push_back(d);
        }
    }
    return list;
}

} // namespace cad::document
