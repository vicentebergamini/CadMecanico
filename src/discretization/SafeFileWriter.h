// Gravação segura de arquivos no Windows (E5.7.4b.8c.2). Sem Qt; implementação Win32 (kernel32).
//
// writeFileSafely grava bytes arbitrários em um arquivo de destino por meio de um temporário próprio,
// criado de forma exclusiva no MESMO diretório do destino, e só então publica o temporário com
// MoveFileExW. Os bytes são opacos: este componente não sabe (nem declara) o que eles representam;
// a validação do conteúdo (por exemplo, composeDemDyna) é responsabilidade de quem chama.
//
// Sequência:
//   1. Validação     opções e cancelamento;
//   2. PathCheck     caminho não vazio; TODO o caminho bruto (representação nativa completa, pelo
//                    comprimento real da string, em todos os componentes e não só no nome) é
//                    examinado e caracteres de controle de 0 a 31, incluindo NUL, são recusados
//                    (UnsupportedPath) ANTES da resolução do caminho absoluto e de qualquer chamada
//                    Win32 que interprete a string como terminada em NUL (E5.7.4b.8c.4d); nada é
//                    corrigido nem removido; sem prefixos \\?\ \\.\ \??\, nome de arquivo válido (sem
//                    caracteres proibidos, sem ':' (fluxo alternativo), sem nome reservado de
//                    dispositivo, sem '.'/' ' final), convertido para absoluto, comprimento compatível
//                    com MAX_PATH, diretório pai existente; destino: inexistente, ou arquivo comum
//                    (recusa diretório, reparse point/link simbólico e somente leitura); com
//                    ReplacePolicy::Forbid um destino existente é recusado já aqui;
//   3. CreateTemp    "<destino>.<pid hex>-<aleatório 64 bits hex>.tmp" com CreateFileW(CREATE_NEW,
//                    compartilhamento 0); colisão de nome → novo nome (no máximo 16 tentativas);
//                    nunca trunca nem abre um arquivo preexistente;
//   4. WriteTemp     WriteFile em blocos de kSafeWriteChunkBytes, tratando escritas parciais;
//                    cancelamento consultado antes de cada bloco; total e tamanho final conferidos;
//   5. FlushTemp     FlushFileBuffers e CloseHandle (falha em qualquer um = falha da gravação);
//   6. VerifyTemp    (padrão) reabre o temporário, confere a identidade (mesmo arquivo criado no
//                    passo 3), o tamanho e TODOS os bytes, e o fim de arquivo;
//   7. Publish       cancelamento consultado; nova conferência do temporário imediatamente antes da
//                    publicação (E5.7.4b.8c.3): o arquivo no caminho do temporário deve ser o mesmo
//                    criado no passo 3 (volume e índice NTFS), comum (sem reparse point), com o
//                    tamanho esperado e — se houve verificação — a mesma data de modificação relida
//                    no passo 6; caso contrário (Publish, TempChanged) nada é publicado e o arquivo
//                    encontrado não é removido se não for o nosso. A conferência REDUZ, mas não elimina,
//                    a janela entre o fechamento e a publicação (resta o intervalo entre a conferência
//                    e a MoveFileExW, que opera por caminho);
//                    MoveFileExW(temporário, destino, MOVEFILE_WRITE_THROUGH
//                    [| MOVEFILE_REPLACE_EXISTING somente com ReplacePolicy::Allow]).
//                    MOVEFILE_COPY_ALLOWED nunca é usado (entre volumes a publicação falha em vez de
//                    virar cópia + exclusão). O destino nunca é excluído antes da publicação.
//   Falha em qualquer etapa: o temporário próprio é removido (somente ele: a remoção confere a
//   identidade do arquivo — volume e índice NTFS — e usa o próprio handle); o destino não é tocado.
//
// Três casos de destino:
//   - inexistente                         → criado na publicação;
//   - existente, ReplacePolicy::Forbid    → recusado (PathCheck, DestinationExists). Se o destino
//     surgir entre a verificação e a publicação, a própria MoveFileExW (sem REPLACE_EXISTING) recusa
//     (Publish, DestinationExists): a recusa não depende só da verificação prévia;
//   - existente, ReplacePolicy::Allow     → substituído pela MoveFileExW; se ela falhar, o anterior
//     permanece.
//
// Resultado (SafeWriteResult):
//   - ok = true somente se escrita, conferência, flush, fechamento, verificação e publicação
//     terminaram sem erro;
//   - stage/error: etapa e motivo da (primeira) falha; Done/None em caso de sucesso. A falha de
//     limpeza do temporário não substitui a falha original: é informada em tempRemoved,
//     cleanupWin32Error e na mensagem (WriteStage::Cleanup não é usado como etapa principal);
//   - win32Error: GetLastError da chamada que falhou (0 quando não se aplica);
//   - bytes: bytes confirmados por WriteFile no temporário;
//   - destinationExisted: o destino existia na verificação de caminhos;
//   - destinationReplaced: publicação com Allow sobre um destino que existia na verificação;
//   - destinationPreserved: nenhum arquivo PREEXISTENTE no caminho de destino foi substituído ou
//     removido por esta operação, quando isso pode ser confirmado (E5.7.4b.8c.4d: esclarecimento; o
//     comportamento não mudou). O campo não afirma que uma ausência anterior continua: depois de uma
//     criação bem-sucedida o destino passa a existir, e o campo só indica que nenhum arquivo anterior
//     foi sobrescrito. Antes da publicação é verdadeiro por construção (o destino não é aberto para
//     escrita, nem excluído, nem renomeado). Após falha da publicação só é verdadeiro se for
//     CONFIRMADO: o temporário continua no lugar (mesma identidade) e, se o destino existia, ele tem a
//     mesma identidade, tamanho, data de modificação e atributos registrados antes. Se não for possível
//     confirmar, é falso. Em sucesso com Forbid: verdadeiro (a MoveFileExW sem REPLACE_EXISTING só tem
//     sucesso se não havia arquivo no destino, portanto nenhum foi substituído). Em sucesso com Allow:
//     falso, pois não há como comprovar que nenhum arquivo foi substituído (um destino criado por
//     terceiro depois da verificação também seria substituído);
//   - tempRemoved: nenhum temporário desta operação ficou em tempPath (não criado, publicado,
//     removido, ou ausente do caminho quando a limpeza foi feita). Falso → tempPath indica o arquivo a remover manualmente, EXCETO quando
//     tempPathForeign for verdadeiro;
//   - tempPathForeign: o arquivo encontrado em tempPath não é o temporário criado por esta operação
//     (foi substituído por terceiro). Ele NÃO foi removido e não deve ser excluído como se fosse o
//     temporário; o paradeiro do temporário original é desconhecido (tempRemoved = false).
//
// Estados do resultado (E5.7.4b.8c.4c):
//   - gravação não iniciada: falha em Validation ou PathCheck; nenhum temporário (tempPath vazio) e o
//     destino não foi aberto para escrita;
//   - gravação iniciada sem publicação: ok = false com stage entre CreateTemp e Publish (inclusive
//     error = Internal); o destino não foi substituído por esta operação — exceto se a própria
//     MoveFileExW falhou, caso em que destinationPreserved só é verdadeiro se confirmado (acima);
//   - publicação confirmada: ok = true, stage = Done, error = None. stage = Done é atribuída SOMENTE
//     depois de uma MoveFileExW bem-sucedida e SEMPRE com ok = true: não existe resultado com
//     stage = Done e ok = false. Depois desse ponto nada mais é feito que possa alocar ou lançar (a
//     mensagem de sucesso é montada antes da publicação); sem memória para montá-la, a mensagem é um
//     texto curto ("arquivo criado" ou "substituído"), mas o resultado continua sendo de sucesso. Isso
//     não amplia as garantias de atomicidade e durabilidade descritas em "Limitações";
//   - error = Internal: exceção C++ capturada (por exemplo, std::bad_alloc) ANTES da publicação; stage
//     é a etapa em andamento; o temporário é removido como nas demais falhas (tempRemoved e
//     cleanupWin32Error refletem o que foi confirmado); a mensagem pode ser o texto curto "erro
//     interno" se não houver memória para o detalhamento.
//
// Exceções: writeFileSafely não deixa sair exceções C++ lançadas durante a gravação; os tratadores não
// alocam fora de blocos try. Não há garantia de diagnóstico textual completo sob esgotamento de
// memória. Falhas estruturadas do Windows (SEH: violação de acesso, estouro de pilha) não são exceções
// C++ e não são tratadas. Em compilações Debug do MSVC (_ITERATOR_DEBUG_LEVEL != 0) os próprios
// construtores das strings e caminhos do resultado alocam um bloco interno de depuração; uma falha
// dessa alocação está fora deste contrato (em Release essas construções não alocam).
//
// Limitações (não há garantia contratual absoluta):
//   - atomicidade: a Microsoft não documenta MoveFileExW como atômica. No NTFS, mesmo volume e sem
//     COPY_ALLOWED, a publicação é uma renomeação de metadados registrada no journal; em
//     compartilhamentos de rede (SMB), pastas sincronizadas e FAT/exFAT o comportamento pode diferir;
//   - durabilidade: FlushFileBuffers + MOVEFILE_WRITE_THROUGH reduzem, mas não eliminam, o risco em
//     queda de energia (não há fsync de diretório no Win32; o cache do próprio disco está fora do
//     controle do programa);
//   - corridas: as verificações de caminho (diretório pai, tipo e atributos do destino) são feitas
//     ANTES da publicação; um processo com acesso ao diretório pode trocar o diretório, criar um link
//     ou reparse point ou alterar atributos nesse intervalo. Não há proteção contra troca maliciosa
//     de diretórios/links (a recusa de reparse point vale para o destino no momento da verificação;
//     junções no caminho do diretório são seguidas). O que é garantido pela própria publicação é
//     apenas a recusa de substituição com Forbid e a não utilização de cópia entre volumes;
//   - entre o fechamento do temporário e a publicação, outro processo com acesso ao diretório pode
//     abrir o temporário (a verificação reabre com compartilhamento somente de leitura);
//   - substituir troca a identidade do arquivo: ACL, atributos e fluxos alternativos do arquivo
//     anterior não são preservados; o arquivo publicado herda a ACL do temporário;
//   - links rígidos (E5.7.4b.8c.4d): se o arquivo substituído tinha outros links rígidos (outros nomes
//     para o mesmo arquivo), a substituição por renomeação desfaz apenas o vínculo do NOME de destino;
//     os outros nomes continuam apontando para o conteúdo antigo, que não é alterado;
//   - caminhos com mais de MAX_PATH − 1 caracteres (contando o nome do temporário) são recusados;
//     os prefixos \\?\ e \\.\ não são aceitos;
//   - não há novas tentativas automáticas (antivírus/indexadores podem manter o destino aberto);
//   - concorrência: duas gravações simultâneas para o mesmo destino não se misturam (cada uma usa o
//     próprio temporário e publica por renomeação); com Forbid no máximo uma publica; com Allow a
//     última publicação substitui a anterior (não há bloqueio entre gravações nem ordem garantida);
//   - cancelamento: consultado antes de cada bloco e imediatamente antes da publicação. Depois de
//     iniciada a MoveFileExW o cancelamento não tem efeito e não há reversão.
//
// Injeção de falhas (somente testes): com CAD_SAFE_WRITER_FAULT_INJECTION definido na compilação
// de SafeFileWriter.cpp (somente no executável de teste, nunca em cad_discretization), um gancho por
// thread pode simular falhas de API em pontos internos (safe_write_testing abaixo). Sem essa macro o
// gancho não existe e nada dele é declarado.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
#include <functional>
#endif
#include <string>
#include <string_view>

namespace cad::discretization {

enum class ReplacePolicy { Forbid, Allow };

enum class WriteStage {
    Validation,
    PathCheck,
    CreateTemp,
    WriteTemp,
    FlushTemp,
    VerifyTemp,
    Publish,
    Cleanup,
    Done
};

// Motivo da falha (complementa stage e win32Error).
enum class SafeWriteError {
    None,
    InvalidOptions,            // valor de ReplacePolicy desconhecido
    Cancelled,                 // cancelamento solicitado
    EmptyPath,                 // destino vazio ou sem nome de arquivo
    UnsupportedPath,           // prefixo \\?\ ou \\.\, nome inválido, reservado ou fluxo alternativo
    PathTooLong,               // destino + nome do temporário ≥ MAX_PATH
    DirectoryMissing,          // diretório do destino inexistente
    ParentNotDirectory,        // o "diretório" do destino é um arquivo
    DestinationIsDirectory,
    DestinationIsReparsePoint, // link simbólico, junção ou outro reparse point
    DestinationReadOnly,       // atributo somente leitura (nunca removido)
    DestinationExists,         // existente com ReplacePolicy::Forbid
    DestinationInaccessible,   // não foi possível consultar o destino ou o diretório
    TempCreateFailed,
    WriteFailed,
    SizeMismatch,
    FlushFailed,
    CloseFailed,
    VerifyFailed,              // erro de E/S ou identidade na releitura
    VerifyMismatch,            // bytes relidos diferentes dos gravados
    PublishFailed,
    TempChanged,               // temporário substituído ou alterado antes da publicação
    Internal                  // exceção inesperada (por exemplo, falta de memória)
};

// Tamanho do bloco de escrita e de releitura.
inline constexpr std::size_t kSafeWriteChunkBytes = std::size_t{1} << 20;

struct SafeWriteOptions
{
    ReplacePolicy replace = ReplacePolicy::Forbid; // sobrescrita só com autorização explícita
    bool verifyAfterWrite = true;                  // reabre o temporário e compara todos os bytes
    const std::atomic<bool>* cancel = nullptr;     // consultado antes de cada bloco e da publicação
};

struct SafeWriteResult
{
    bool ok = false;
    WriteStage stage = WriteStage::Validation;
    SafeWriteError error = SafeWriteError::None;
    std::uint32_t win32Error = 0;        // GetLastError da operação que falhou; 0 se não se aplica
    std::string message;                 // UTF-8, em português, com caminho e motivo
    bool destinationExisted = false;
    bool destinationReplaced = false;
    bool destinationPreserved = true;
    bool tempRemoved = true;
    bool tempPathForeign = false;        // tempPath contém arquivo de terceiro (não removido)
    std::uintmax_t bytes = 0;            // bytes confirmados por WriteFile
    std::uint32_t cleanupWin32Error = 0; // erro da remoção do temporário, se houver
    std::filesystem::path destination;   // caminho absoluto usado
    std::filesystem::path tempPath;      // temporário criado (vazio se nenhum foi criado)
};

// Grava 'bytes' em 'destination' (relativo ao diretório atual ou absoluto) conforme o contrato acima.
// Não lança exceções C++ (ver "Exceções" acima); não altera atributos nem permissões; não cria
// diretórios; não impõe extensão.
SafeWriteResult writeFileSafely(const std::filesystem::path& destination, std::string_view bytes,
                                const SafeWriteOptions& options);

const char* writeStageName(WriteStage stage);
const char* safeWriteErrorName(SafeWriteError error);

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION) || defined(CAD_SAFE_WRITER_IMPLEMENTATION)
// Tipos dos pontos de injeção de falhas. Visíveis somente ao executável de teste (macro
// CAD_SAFE_WRITER_FAULT_INJECTION) e à própria implementação; sem a macro, a implementação sempre
// prossegue normalmente e setFaultHook não existe.
namespace safe_write_testing {

enum class FaultPoint {
    TempName,        // antes de cada CreateFileW do temporário (candidate pode ser trocado)
    CreateTemp,      // CreateFileW(CREATE_NEW) do temporário
    AfterCreateTemp, // temporário criado e identificado, antes da escrita
    WriteChunk,      // cada WriteFile (chunk = índice da chamada)
    Flush,           // FlushFileBuffers
    Close,           // CloseHandle após o flush (o handle é fechado de qualquer forma)
    BeforeVerify,    // temporário fechado, antes da reabertura
    ReopenVerify,    // CreateFileW da reabertura
    CompareChunk,    // cada bloco relido, antes da comparação (CorruptRead altera a cópia lida)
    BeforePublish,   // depois da verificação, antes do cancelamento e da conferência final
    Publish,         // MoveFileExW (moveFlags = flags que serão usadas)
    Cleanup          // marcação de exclusão do temporário (FileDispositionInfo)
};

enum class FaultAction {
    Proceed,     // executa a chamada real
    Fail,        // a chamada não é executada e "falha" com win32Error
    Partial,     // WriteChunk: escreve somente partialBytes (> 0) do bloco
    ZeroWrite,   // WriteChunk: WriteFile "tem sucesso" sem escrever nada
    CorruptRead  // CompareChunk: inverte o primeiro byte da cópia relida
};

struct FaultContext
{
    FaultPoint point = FaultPoint::TempName;
    std::size_t chunk = 0;
    std::uint32_t moveFlags = 0;
    const std::wstring* destination = nullptr;
    const std::wstring* tempPath = nullptr;
    std::wstring* candidate = nullptr; // TempName: nome do temporário a tentar (modificável)
};

struct FaultDecision
{
    FaultAction action = FaultAction::Proceed;
    std::uint32_t win32Error = 0;
    std::uint32_t partialBytes = 0;
};

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
using FaultHook = std::function<FaultDecision(const FaultContext&)>;
// Gancho da thread atual (thread_local); vazio = sem injeção.
void setFaultHook(FaultHook hook);
#endif

} // namespace safe_write_testing
#endif

} // namespace cad::discretization
