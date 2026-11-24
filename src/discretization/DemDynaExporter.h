// Exportação do arquivo APDL/LS-DYNA DEM-DYNA para o disco (E5.7.4b.8c.4). Sem Qt.
//
// Liga as duas etapas existentes, sem recalcular nada:
//   1. composeDemDyna(const DemDynaInput&)  (DemDynaApdl.h): valida os dados primários e compõe o
//      texto em memória; com qualquer erro devolve ok = false e nenhum texto;
//   2. writeFileSafely(destino, bytes, opções) (SafeFileWriter.h): temporário exclusivo no diretório
//      do destino, verificação e publicação por MoveFileExW.
//
// Fluxo de exportDemDyna:
//   - a composição é executada primeiro; se ela falhar (ok = false, texto vazio, ou exceção), a
//     exportação termina IMEDIATAMENTE: writeFileSafely não é chamada, nenhum temporário é criado e o
//     destino não é aberto, consultado, criado nem alterado (stage = Composition, writeStarted = false);
//   - somente com a composição aceita (ok = true e texto não vazio) os bytes do compositor são
//     entregues ao gravador, sem cópia transformada, sem conversão de fim de linha e sem codificação
//     adicional (o arquivo final é byte a byte composition.text);
//   - as opções de gravação são repassadas sem alteração: a política de substituição recebida
//     (ReplacePolicy::Forbid por padrão; Allow somente se o chamador a fornecer), a verificação e o
//     cancelamento. Não há confirmação de sobrescrita nem novas tentativas automáticas.
//
// A entrada não é modificada (composeDemDyna recebe referência constante).
//
// Resultado (DemDynaExportResult):
//   - ok = true somente se a composição foi aceita E a gravação publicou o arquivo (write.ok);
//     qualquer outra combinação é falha (não há sucesso parcial);
//   - stage: Composition (composição recusada; gravação não iniciada), Write (composição aceita e
//     gravação falhou) ou Done;
//   - composition: diagnósticos completos da composição (errors, lawReport, contagens). O texto é
//     mantido somente quando a composição foi aceita; numa composição recusada ele é sempre vazio;
//   - writeStarted: writeFileSafely foi chamada. Falso → 'write' não descreve nenhuma gravação: ok =
//     false, error = None, bytes = 0, tempPath vazio, tempRemoved = true (nenhum temporário existe) e
//     destinationPreserved = true (o destino não foi tocado por construção); a mensagem diz que a
//     gravação não foi iniciada;
//   - write: SafeWriteResult integral do gravador quando writeStarted (etapa, motivo, Win32, estado do
//     destino, tempRemoved/tempPathForeign/cleanupWin32Error — falhas de limpeza não são ocultadas);
//   - destination: caminho absoluto usado pelo gravador (ou o caminho recebido, se a gravação não foi
//     iniciada);
//   - bytesComposed: tamanho do texto aceito (0 se a composição foi recusada);
//   - bytesWritten: bytes confirmados por WriteFile no temporário (write.bytes);
//   - bytesPublished: bytes do arquivo publicado (= bytesComposed em sucesso; 0 em qualquer falha);
//   - message: resumo em português (UTF-8) com a etapa da falha.
#pragma once

#include "discretization/DemDynaApdl.h"
#include "discretization/SafeFileWriter.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace cad::discretization {

enum class DemDynaExportStage { Composition, Write, Done };

struct DemDynaExportResult
{
    bool ok = false;
    DemDynaExportStage stage = DemDynaExportStage::Composition;
    DemDynaComposition composition;
    bool writeStarted = false;
    SafeWriteResult write;
    std::filesystem::path destination;
    std::uintmax_t bytesComposed = 0;
    std::uintmax_t bytesWritten = 0;
    std::uintmax_t bytesPublished = 0;
    std::string message;
};

// Compõe o arquivo DEM-DYNA e, somente se a composição for aceita, grava-o com writeFileSafely.
//
// Exceções (E5.7.4b.8c.4d: alinhado às limitações de SafeFileWriter.h; a função não é noexcept):
//   - exceções C++ (por exemplo, std::bad_alloc) lançadas durante a composição, a gravação ou a
//     montagem do resultado são capturadas e não saem de exportDemDyna; o resultado descreve o estado
//     (composição recusada, gravação não iniciada, não confirmada ou falha do gravador);
//   - falhas estruturadas do Windows (SEH: violação de acesso, estouro de pilha) não são exceções C++
//     e não são tratadas;
//   - em compilações Debug do MSVC (_ITERATOR_DEBUG_LEVEL != 0), alocações internas de depuração feitas
//     dentro de construtores/movimentos noexcept da biblioteca padrão, se falharem, encerram o processo
//     (std::terminate) em vez de lançar; esse caso está fora deste contrato;
//   - sob esgotamento de memória não há garantia de diagnóstico completo: as mensagens podem ser
//     substituídas por textos curtos de reserva ou ficar vazias, mas os campos de estado continuam
//     coerentes.
DemDynaExportResult exportDemDyna(const DemDynaInput& input, const std::filesystem::path& destination,
                                  const SafeWriteOptions& options = SafeWriteOptions{});

const char* demDynaExportStageName(DemDynaExportStage stage);

} // namespace cad::discretization
