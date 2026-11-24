// Exportação DEM-DYNA: composição (DemDynaApdl) + gravação segura (SafeFileWriter). Contrato em
// DemDynaExporter.h (E5.7.4b.8c.4).
//
// Contrato de exceções (E5.7.4b.8c.4b): nenhuma exceção sai de exportDemDyna.
//   - Todo o trabalho que pode lançar (composição, gravação, cópias de caminho, montagem de
//     mensagens) ocorre dentro de blocos try.
//   - Os tratadores só executam operações que não lançam (bool, inteiros, enumerações, clear(),
//     swap()) e tentativas de alocação protegidas por try próprio; se até o texto curto de reserva
//     falhar, a mensagem fica vazia, mas os campos do resultado continuam coerentes.
//   - Os campos de estado são confirmados por operações que não lançam; as mensagens são montadas
//     antes da confirmação e só então trocadas (swap), de modo que uma falha de alocação nunca deixa
//     o resultado meio atualizado.
//   - writeFileSafely é chamada no máximo uma vez; nenhum tratador grava, remove ou repete gravação.
#include "discretization/DemDynaExporter.h"

#include <exception>
#include <string>
#include <utility>

namespace cad::discretization {
namespace {

namespace fs = std::filesystem;

// Textos de reserva com no máximo 15 bytes UTF-8: cabem no buffer interno de std::string (MSVC),
// de modo que, na prática, sua atribuição não aloca. Ainda assim são atribuídos dentro de try.
constexpr const char* kShortNotWritten = "não gravado";       // 11 bytes
constexpr const char* kShortNotStarted = "não iniciada";      // 13 bytes
constexpr const char* kShortUnconfirmed = "não confirmado";   // 15 bytes
constexpr const char* kShortWriteFailed = "escrita falhou";   // 14 bytes
constexpr const char* kShortPublished = "publicado";          //  9 bytes

// Executa 'build' e devolve o texto montado; se a montagem lançar (por exemplo, std::bad_alloc),
// devolve texto vazio. Nunca lança.
template <class Build>
std::string tryBuild(Build&& build)
{
    try {
        return build();
    } catch (...) {
        return std::string();
    }
}

// Coloca 'built' em 'out' (swap, sem alocação); se 'built' estiver vazio, tenta o texto curto e,
// em último caso, deixa 'out' vazio. Nunca lança.
void setMessage(std::string& out, std::string&& built, const char* shortText)
{
    if (!built.empty()) {
        out.swap(built);
        return;
    }
    try {
        out.assign(shortText);
    } catch (...) {
        out.clear();
    }
}

// Copia o caminho, se possível; em falha mantém o valor anterior (atribuição de path tem garantia
// forte). Nunca lança.
void tryAssignPath(fs::path& out, const fs::path& value)
{
    try {
        out = value;
    } catch (...) {
    }
}

// Resultado de gravação que declara explicitamente que writeFileSafely NÃO foi chamada. As
// garantias sobre destino e temporário decorrem da construção (a gravação não começou), não de
// suposição: o destino não foi aberto, consultado, criado nem alterado e nenhum temporário existe.
// Nunca lança.
void markWriteNotStarted(SafeWriteResult& w, const fs::path& destination, const char* why)
{
    w.ok = false;
    w.stage = WriteStage::Validation;
    w.error = SafeWriteError::None;
    w.win32Error = 0;
    w.destinationExisted = false;   // o destino não foi consultado
    w.destinationReplaced = false;
    w.destinationPreserved = true;  // writeFileSafely não foi chamada
    w.tempRemoved = true;           // nenhum temporário foi criado
    w.tempPathForeign = false;
    w.bytes = 0;
    w.cleanupWin32Error = 0;
    w.tempPath.clear();
    tryAssignPath(w.destination, destination);
    setMessage(w.message, tryBuild([&] { return std::string("gravação não iniciada: ") + why; }), kShortNotStarted);
}

// writeFileSafely foi chamada, mas lançou em vez de devolver um resultado (contrato do gravador
// violado; por exemplo, std::bad_alloc na construção do gravador ou no seu próprio tratamento de
// erro). Nada se sabe sobre o que ela fez: não se afirma preservação do destino, remoção de
// temporário nem ausência de publicação. Nenhuma limpeza é tentada aqui. Nunca lança.
void markWriteUnconfirmed(SafeWriteResult& w, const fs::path& destination, const char* what)
{
    w.ok = false;
    w.stage = WriteStage::Validation; // etapa não determinada (não há valor "desconhecida")
    w.error = SafeWriteError::Internal;
    w.win32Error = 0;
    w.destinationExisted = false;
    w.destinationReplaced = false;
    w.destinationPreserved = false;   // não confirmado
    w.tempRemoved = false;            // não confirmado (tempPath desconhecido)
    w.tempPathForeign = false;
    w.bytes = 0;
    w.cleanupWin32Error = 0;
    w.tempPath.clear();
    tryAssignPath(w.destination, destination);
    setMessage(w.message, tryBuild([&] {
                   return std::string("estado da gravação não confirmado: a gravação segura lançou exceção (") +
                          what +
                          "); não é possível afirmar que o destino foi preservado, que ele não foi criado ou "
                          "substituído, nem que nenhum temporário restou no diretório";
               }),
               kShortUnconfirmed);
}

std::string issueSummary(const DemDynaComposition& c)
{
    if (c.errors.empty())
        return c.lawReport.blocksExport() ? "leis constitutivas inválidas" : "motivo não informado";
    const DemDynaIssue& first = c.errors.front();
    std::string s = "[" + first.stage + "] " + first.message;
    if (c.errors.size() > 1)
        s += " (+" + std::to_string(c.errors.size() - 1) + " erro(s))";
    return s;
}

// Registra um erro de composição, se houver memória para isso. Nunca lança.
void tryAddIssue(DemDynaComposition& c, const char* prefix, const char* what)
{
    try {
        c.errors.push_back({"entrada", 0, std::string(prefix) + what}); // garantia forte
    } catch (...) {
    }
}

// Composição recusada (ou interrompida) antes da gravação. Os campos de estado são confirmados
// primeiro, sem alocação; os textos vêm depois, cada um com reserva. Nunca lança.
// 'internalWhat' (opcional): diagnóstico da exceção, colocado diretamente na mensagem para não
// depender de o registro em composition.errors ter tido memória disponível.
void compositionRefused(DemDynaExportResult& r, const fs::path& destination, const char* writeWhy,
                        const char* messagePrefix, const char* internalWhat = nullptr)
{
    r.ok = false;
    r.stage = DemDynaExportStage::Composition;
    r.composition.ok = false;
    r.composition.text.clear(); // nenhum texto utilizável de composição recusada
    r.writeStarted = false;
    r.bytesComposed = 0;
    r.bytesWritten = 0;
    r.bytesPublished = 0;
    markWriteNotStarted(r.write, destination, writeWhy);
    tryAssignPath(r.destination, destination);
    setMessage(r.message, tryBuild([&] {
                   return internalWhat != nullptr ? std::string(messagePrefix) + internalWhat
                                                  : std::string(messagePrefix) + issueSummary(r.composition);
               }),
               kShortNotWritten);
}

constexpr const char* kRefusedPrefix =
    "exportação DEM-DYNA recusada na composição (nenhum arquivo foi gravado): ";
constexpr const char* kInterruptedPrefix =
    "exportação DEM-DYNA interrompida por erro interno antes da gravação (gravação não iniciada; nenhum "
    "arquivo foi gravado): ";

// Exceção antes de writeFileSafely (P2). Nunca lança e nunca grava.
void failedBeforeWrite(DemDynaExportResult& r, const fs::path& destination, const char* what)
{
    tryAddIssue(r.composition, "erro interno antes da gravação: ", what);
    compositionRefused(r, destination, "erro interno antes da gravação", kInterruptedPrefix, what);
}

// writeFileSafely lançou (P3). Nunca lança e nunca grava.
void writeThrew(DemDynaExportResult& r, const fs::path& destination, const char* what)
{
    r.ok = false;
    r.stage = DemDynaExportStage::Write;
    r.writeStarted = true;
    r.bytesWritten = 0;
    r.bytesPublished = 0;
    markWriteUnconfirmed(r.write, destination, what);
    tryAssignPath(r.destination, destination);
    setMessage(r.message, tryBuild([&] {
                   return std::string("exportação DEM-DYNA: composição aceita (") + std::to_string(r.bytesComposed) +
                          " bytes); " + r.write.message;
               }),
               kShortUnconfirmed);
}

// Conclusão a partir do resultado devolvido por writeFileSafely (P1). Nunca lança e nunca grava.
void finishWrite(DemDynaExportResult& r)
{
    r.bytesWritten = r.write.bytes;
    if (!r.write.destination.empty())
        tryAssignPath(r.destination, r.write.destination); // em falha, mantém o caminho recebido

    if (!r.write.ok) {
        // O gravador só usa a etapa Done depois de uma MoveFileExW bem-sucedida. Desde a E5.7.4b.8c.4c o
        // contrato do gravador exclui stage = Done com ok = false (nada que aloque ou lance é feito depois
        // da publicação), portanto este ramo é DEFENSIVO: se essa combinação voltar a aparecer, significa
        // que o arquivo FOI publicado e o gravador falhou depois. O resultado segue o do gravador (falha,
        // bytesPublished = 0), mas a mensagem não descreve a publicação como não ocorrida.
        const bool publishedThenFailed = r.write.stage == WriteStage::Done;
        std::string msg = tryBuild([&] {
            std::string s = std::string("exportação DEM-DYNA: composição aceita (") + std::to_string(r.bytesComposed) +
                            " bytes), gravação falhou [" + writeStageName(r.write.stage) + "/" +
                            safeWriteErrorName(r.write.error) + "]";
            if (publishedThenFailed)
                s += " DEPOIS da publicação: o arquivo de destino foi criado/substituído, mas o gravador não "
                     "concluiu o próprio resultado";
            return s + ": " + r.write.message;
        });
        r.ok = false;
        r.stage = DemDynaExportStage::Write;
        r.bytesPublished = 0;
        setMessage(r.message, std::move(msg), publishedThenFailed ? kShortPublished : kShortWriteFailed);
        return;
    }

    // Publicação concluída (fato consumado). A mensagem é montada ANTES de confirmar o estado final; se a
    // montagem falhar, o estado continua sendo de sucesso (o arquivo existe e foi verificado pelo
    // gravador) e a mensagem informa que o detalhamento não pôde ser montado, com o motivo.
    std::string msg;
    try {
        msg = "exportação DEM-DYNA concluída: " + r.write.message;
    } catch (const std::exception& e) {
        const char* what = e.what();
        msg = tryBuild([&] {
            return std::string("exportação DEM-DYNA concluída: arquivo publicado (") + std::to_string(r.bytesComposed) +
                   " bytes); mensagem detalhada indisponível: " + what;
        });
    } catch (...) {
        msg = tryBuild([&] {
            return std::string("exportação DEM-DYNA concluída: arquivo publicado (") + std::to_string(r.bytesComposed) +
                   " bytes); mensagem detalhada indisponível: exceção desconhecida";
        });
    }
    r.ok = true;
    r.stage = DemDynaExportStage::Done;
    r.bytesPublished = r.bytesComposed;
    setMessage(r.message, std::move(msg), kShortPublished);
}

} // namespace

DemDynaExportResult exportDemDyna(const DemDynaInput& input, const std::filesystem::path& destination,
                                  const SafeWriteOptions& options)
{
    DemDynaExportResult r;
    bool writeReturned = false;
    try {
        // Caminho informado desde o início (antes da composição e de qualquer gravação).
        r.destination = destination;

        // 1. Composição.
        try {
            r.composition = composeDemDyna(input);
        } catch (const std::exception& e) {
            tryAddIssue(r.composition, "erro interno na composição: ", e.what());
            compositionRefused(r, destination, "composição DEM-DYNA recusada", kRefusedPrefix);
            return r;
        } catch (...) {
            tryAddIssue(r.composition, "erro interno na composição: ", "exceção desconhecida");
            compositionRefused(r, destination, "composição DEM-DYNA recusada", kRefusedPrefix);
            return r;
        }
        if (!r.composition.ok) {
            compositionRefused(r, destination, "composição DEM-DYNA recusada", kRefusedPrefix);
            return r;
        }
        if (r.composition.text.empty()) {
            // Defensivo: composição "aceita" sem texto não é publicável.
            tryAddIssue(r.composition, "composição aceita sem texto ", "(inconsistência interna)");
            compositionRefused(r, destination, "composição DEM-DYNA recusada", kRefusedPrefix);
            return r;
        }

        // 2. Gravação dos bytes do compositor, com as opções recebidas sem alteração. Daqui em diante
        // nada lança fora dos blocos locais: writeFileSafely é chamada uma única vez.
        r.bytesComposed = r.composition.text.size();
        r.stage = DemDynaExportStage::Write;
        r.writeStarted = true;
        try {
            r.write = writeFileSafely(destination, r.composition.text, options); // atribuição por movimento
            writeReturned = true;
        } catch (const std::exception& e) {
            writeThrew(r, destination, e.what());
            return r;
        } catch (...) {
            writeThrew(r, destination, "exceção desconhecida");
            return r;
        }
        finishWrite(r);
    } catch (const std::exception& e) {
        // Defensivo: com a estrutura acima só a cópia inicial do caminho chega aqui.
        if (!r.writeStarted)
            failedBeforeWrite(r, destination, e.what());
        else if (!writeReturned)
            writeThrew(r, destination, e.what());
        else
            finishWrite(r);
    } catch (...) {
        if (!r.writeStarted)
            failedBeforeWrite(r, destination, "exceção desconhecida");
        else if (!writeReturned)
            writeThrew(r, destination, "exceção desconhecida");
        else
            finishWrite(r);
    }
    return r;
}

const char* demDynaExportStageName(DemDynaExportStage stage)
{
    switch (stage) {
    case DemDynaExportStage::Composition: return "Composition";
    case DemDynaExportStage::Write: return "Write";
    case DemDynaExportStage::Done: return "Done";
    }
    return "?";
}

} // namespace cad::discretization
