// Gravação segura de arquivos no Windows (E5.7.4b.8c.2; pontos de injeção de falhas e conferência
// final do temporário: E5.7.4b.8c.3). Contrato e limitações: SafeFileWriter.h.
#define CAD_SAFE_WRITER_IMPLEMENTATION
#include "discretization/SafeFileWriter.h"

#ifndef _WIN32
#error "SafeFileWriter: implementação disponível somente para Windows (Win32)."
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <random>
#include <system_error>
#include <utility>
#include <vector>

namespace cad::discretization {
namespace {

namespace fs = std::filesystem;
using namespace safe_write_testing;

// ---------------------------------------------------------------------------------------------
// Pontos de injeção de falhas. Sem CAD_SAFE_WRITER_FAULT_INJECTION, probe() sempre devolve Proceed:
// não existe gancho e as chamadas reais são sempre executadas.
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
thread_local FaultHook tlFaultHook;
#endif

FaultDecision probe(FaultPoint point, const std::wstring* destination, const std::wstring* tempPath,
                    std::size_t chunk = 0, std::uint32_t moveFlags = 0, std::wstring* candidate = nullptr)
{
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    if (tlFaultHook) {
        FaultContext ctx;
        ctx.point = point;
        ctx.chunk = chunk;
        ctx.moveFlags = moveFlags;
        ctx.destination = destination;
        ctx.tempPath = tempPath;
        ctx.candidate = candidate;
        return tlFaultHook(ctx);
    }
#endif
    (void)point;
    (void)destination;
    (void)tempPath;
    (void)chunk;
    (void)moveFlags;
    (void)candidate;
    return {};
}

// Falha simulada: a chamada real não é feita e GetLastError passa a devolver o código injetado.
bool injectedFailure(const FaultDecision& d)
{
    if (d.action != FaultAction::Fail)
        return false;
    ::SetLastError(d.win32Error);
    return true;
}

// Textos de reserva (E5.7.4b.8c.4c), usados quando a mensagem detalhada não pôde ser montada (por
// exemplo, sem memória). Têm no máximo 15 bytes UTF-8: cabem no buffer interno de std::string, de modo
// que a atribuição a uma string existente não aloca; ainda assim é feita dentro de try (assignShort).
constexpr const char* kShortCreated = "arquivo criado";  // 14 bytes
constexpr const char* kShortReplaced = "substituído";    // 12 bytes
constexpr const char* kShortInternal = "erro interno";   // 12 bytes

// Atribui um texto curto de reserva; em último caso deixa a mensagem vazia. Nunca lança.
void assignShort(std::string& out, const char* text)
{
    try {
        out.assign(text);
    } catch (...) {
        out.clear();
    }
}

// Comprimento máximo do sufixo do temporário: "." + 8 hex (pid) + "-" + 16 hex + ".tmp".
constexpr std::size_t kTempSuffixLength = 1 + 8 + 1 + 16 + 4;
constexpr int kTempNameAttempts = 16;

// ---------------------------------------------------------------------------------------------
// Handle com fechamento garantido. close() explícito permite tratar a falha de CloseHandle.
class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle()
    {
        if (valid())
            ::CloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE get() const { return h_; }
    void reset(HANDLE h)
    {
        if (valid())
            ::CloseHandle(h_);
        h_ = h;
    }
    // Fecha o handle; false (com GetLastError) se CloseHandle falhar. O handle deixa de ser usado em
    // qualquer caso (não há segunda tentativa de fechamento).
    bool close()
    {
        if (!valid())
            return true;
        HANDLE h = h_;
        h_ = INVALID_HANDLE_VALUE;
        return ::CloseHandle(h) != 0;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Identidade e estado de um arquivo (GetFileInformationByHandle).
struct FileInfo
{
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
    DWORD attributes = 0;
    ULONGLONG size = 0;
    FILETIME lastWrite{};
};

bool sameFile(const FileInfo& a, const FileInfo& b)
{
    return a.volume == b.volume && a.indexHigh == b.indexHigh && a.indexLow == b.indexLow;
}

bool sameSnapshot(const FileInfo& a, const FileInfo& b)
{
    return sameFile(a, b) && a.attributes == b.attributes && a.size == b.size &&
           a.lastWrite.dwLowDateTime == b.lastWrite.dwLowDateTime &&
           a.lastWrite.dwHighDateTime == b.lastWrite.dwHighDateTime;
}

bool infoOf(HANDLE h, FileInfo& info)
{
    BY_HANDLE_FILE_INFORMATION bhfi{};
    if (!::GetFileInformationByHandle(h, &bhfi))
        return false;
    info.volume = bhfi.dwVolumeSerialNumber;
    info.indexHigh = bhfi.nFileIndexHigh;
    info.indexLow = bhfi.nFileIndexLow;
    info.attributes = bhfi.dwFileAttributes;
    info.size = (static_cast<ULONGLONG>(bhfi.nFileSizeHigh) << 32) | bhfi.nFileSizeLow;
    info.lastWrite = bhfi.ftLastWriteTime;
    return true;
}

enum class Presence { Exists, Absent, Error };

// Consulta o caminho sem seguir reparse points e sem pedir acesso aos dados (FILE_READ_ATTRIBUTES não
// conflita com modos de compartilhamento de outros processos).
Presence queryPath(const std::wstring& path, FileInfo& info, DWORD& error)
{
    error = 0;
    Handle h(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid()) {
        error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return Presence::Absent;
        return Presence::Error;
    }
    if (!infoOf(h.get(), info)) {
        error = ::GetLastError();
        return Presence::Error;
    }
    return Presence::Exists;
}

// ---------------------------------------------------------------------------------------------
// Texto.
std::string toUtf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0)
        return "<caminho não representável>";
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Caminho entre aspas, SOMENTE para diagnóstico (E5.7.4b.8c.4d): caracteres de controle (0–31) são
// mostrados como \xNN (NUL → \x00, TAB → \x09, LF → \x0A, CR → \x0D), de modo que a mensagem nunca
// contém NUL bruto nem é truncada por quem a lê como C-string. Os demais caracteres (Unicode válido)
// são convertidos sem alteração. O texto escapado nunca é usado em operações de sistema de arquivos.
std::string quotePath(const std::wstring& path)
{
    static const wchar_t* const kHex = L"0123456789ABCDEF";
    std::wstring shown;
    shown.reserve(path.size());
    for (const wchar_t c : path) {
        if (static_cast<unsigned>(c) < 32u) {
            shown += L"\\x";
            shown += kHex[(c >> 4) & 0xF];
            shown += kHex[c & 0xF];
        } else {
            shown += c;
        }
    }
    return "\"" + toUtf8(shown) + "\"";
}

// Primeiro caractere de controle (0–31, incluindo NUL) na representação nativa COMPLETA do caminho,
// percorrida pelo comprimento real da string (nunca por wcslen ou c_str()); npos se não houver.
std::size_t firstControlChar(const std::wstring& path)
{
    for (std::size_t i = 0; i < path.size(); ++i)
        if (static_cast<unsigned>(path[i]) < 32u)
            return i;
    return std::wstring::npos;
}

std::string systemMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (n > 0 && buffer)
        text.assign(buffer, n);
    if (buffer)
        ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' ||
                             text.back() == L'.'))
        text.pop_back();
    return toUtf8(text);
}

std::string win32Text(DWORD code)
{
    if (code == 0)
        return {};
    std::string s = " [Win32 " + std::to_string(code);
    const std::string sys = systemMessage(code);
    if (!sys.empty())
        s += ": " + sys;
    return s + "]";
}

std::wstring hex(std::uint64_t value, int digits)
{
    static const wchar_t* const kDigits = L"0123456789abcdef";
    std::wstring s(static_cast<std::size_t>(digits), L'0');
    for (int i = digits - 1; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return s;
}

std::uint64_t randomComponent()
{
    try {
        std::random_device rd; // MSVC: rand_s (gerador criptográfico do sistema)
        return (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    } catch (...) {
        LARGE_INTEGER counter{};
        ::QueryPerformanceCounter(&counter);
        return static_cast<std::uint64_t>(counter.QuadPart) ^
               (static_cast<std::uint64_t>(::GetTickCount64()) << 20);
    }
}

// ---------------------------------------------------------------------------------------------
// Nomes.
bool startsWith(std::wstring_view s, std::wstring_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Nomes reservados de dispositivo (CON, PRN, AUX, NUL, COM0-9, LPT0-9, COM¹²³, LPT¹²³, CONIN$,
// CONOUT$): no Windows 10 continuam reservados mesmo com extensão ("CON.inp").
bool reservedDeviceName(std::wstring_view name)
{
    std::wstring base(name.substr(0, name.find(L'.')));
    while (!base.empty() && base.back() == L' ')
        base.pop_back();
    for (auto& c : base)
        if (c >= L'a' && c <= L'z')
            c = static_cast<wchar_t>(c - L'a' + L'A');
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" ||
        base == L"CONOUT$")
        return true;
    if (base.size() == 4 && (startsWith(base, L"COM") || startsWith(base, L"LPT"))) {
        const wchar_t d = base[3];
        return (d >= L'0' && d <= L'9') || d == L'¹' || d == L'²' || d == L'³';
    }
    return false;
}

// Motivo de recusa do nome de arquivo (vazio = aceito).
std::string fileNameProblem(const std::wstring& name)
{
    if (name.empty())
        return "o caminho não indica um arquivo (nome vazio ou separador no final)";
    if (name == L"." || name == L"..")
        return "o caminho não indica um arquivo ('.' ou '..')";
    for (wchar_t c : name) {
        if (c < 32)
            return "o nome do arquivo contém caractere de controle";
        if (c == L':')
            return "o nome do arquivo contém ':' (fluxo alternativo de dados NTFS não é aceito)";
        if (c == L'<' || c == L'>' || c == L'"' || c == L'/' || c == L'\\' || c == L'|' || c == L'?' ||
            c == L'*')
            return "o nome do arquivo contém caractere não permitido no Windows (< > \" / \\ | ? *)";
    }
    if (name.back() == L'.' || name.back() == L' ')
        return "o nome do arquivo termina com '.' ou espaço (o Windows removeria esse caractere)";
    if (reservedDeviceName(name))
        return "o nome do arquivo é um nome reservado de dispositivo do Windows (CON, PRN, AUX, NUL, "
               "COMn, LPTn)";
    return {};
}

// ---------------------------------------------------------------------------------------------
class Writer
{
public:
    Writer(const fs::path& destination, std::string_view bytes, const SafeWriteOptions& options)
        : input_(destination), bytes_(bytes), options_(options)
    {
    }

    SafeWriteResult run()
    {
        try {
            if (validate() && checkPaths() && createTemp() && writeTemp() && flushTemp() && verifyTemp())
                publish();
        } catch (const std::exception& e) {
            internalFailure(e.what());
        } catch (...) {
            internalFailure("exceção desconhecida");
        }
        return std::move(r_);
    }

private:
    // --- resultado ---------------------------------------------------------------------------
    bool fail(WriteStage stage, SafeWriteError error, DWORD win32, const std::string& message)
    {
        r_.ok = false;
        r_.stage = stage;
        r_.error = error;
        r_.win32Error = win32;
        r_.message = message + win32Text(win32);
        return false;
    }

    bool cancelled() const
    {
        return options_.cancel != nullptr && options_.cancel->load(std::memory_order_acquire);
    }

    // Exceção capturada em run() (E5.7.4b.8c.4c: nunca lança e nunca desfaz uma publicação).
    //  - Publicação já confirmada (published_): o fato é preservado — ok, stage = Done e error = None
    //    permanecem; só se garante uma mensagem. Defensivo: depois da MoveFileExW bem-sucedida nenhuma
    //    operação lança (ver publish), portanto este ramo não é alcançado pela implementação atual.
    //  - Antes da publicação: o temporário próprio é removido (as rotinas de limpeza não alocam) e os
    //    campos de falha são confirmados sem alocação; stage continua sendo a etapa em andamento (nunca
    //    Done, atribuída somente após a publicação). A mensagem detalhada é tentada; sem memória, usa o
    //    texto curto de reserva; a nota de limpeza também é opcional (tempRemoved/cleanupWin32Error
    //    continuam informando o estado do temporário).
    void internalFailure(const char* what)
    {
        if (published_) {
            if (r_.message.empty())
                assignShort(r_.message, r_.destinationReplaced ? kShortReplaced : kShortCreated);
            return;
        }
        if (temp_.valid()) {
            // Ainda aberto: exclusão pelo próprio handle (compartilhamento 0 → é o nosso arquivo).
            abandonOpenTemp();
        } else if (tempCreated_) {
            removeClosedTemp();
        }
        r_.ok = false;
        r_.error = SafeWriteError::Internal;
        r_.win32Error = 0;
        std::string message;
        try {
            message = std::string("erro interno durante a gravação segura: ") + what;
        } catch (...) {
            message.clear();
        }
        if (!message.empty())
            r_.message.swap(message);
        else
            assignShort(r_.message, kShortInternal);
        try {
            appendCleanupNote();
        } catch (...) {
            // Sem memória para a nota: o estado do temporário continua nos campos do resultado.
        }
    }

    // --- 1. validação ------------------------------------------------------------------------
    bool validate()
    {
        r_.stage = WriteStage::Validation;
        if (options_.replace != ReplacePolicy::Forbid && options_.replace != ReplacePolicy::Allow)
            return fail(WriteStage::Validation, SafeWriteError::InvalidOptions, 0,
                        "política de substituição inválida");
        if (cancelled())
            return fail(WriteStage::Validation, SafeWriteError::Cancelled, 0,
                        "gravação cancelada antes de iniciar");
        return true;
    }

    // --- 2. caminhos -------------------------------------------------------------------------
    bool checkPaths()
    {
        r_.stage = WriteStage::PathCheck;
        const std::wstring& raw = input_.native();
        if (raw.empty())
            return fail(WriteStage::PathCheck, SafeWriteError::EmptyPath, 0, "caminho de destino vazio");
        // E5.7.4b.8c.4d (A1): caracteres de controle (0–31) em QUALQUER posição do caminho bruto — não só
        // no nome final — são recusados ANTES de fs::absolute e de qualquer API que receba c_str(). Um
        // NUL interno truncaria o caminho nessas APIs (GetFullPathNameW, CreateFileW, MoveFileExW...) e
        // a gravação seria feita num prefixo do caminho pedido. Nada é corrigido nem removido.
        if (const std::size_t pos = firstControlChar(raw); pos != std::wstring::npos) {
            static const char* const kHex = "0123456789ABCDEF";
            const unsigned code = static_cast<unsigned>(raw[pos]);
            const char shown[] = {'\\', 'x', kHex[(code >> 4) & 0xF], kHex[code & 0xF], '\0'};
            return fail(WriteStage::PathCheck, SafeWriteError::UnsupportedPath, 0,
                        std::string("o caminho contém caractere de controle ") + shown + " na posição " +
                            std::to_string(pos) + " (não é aceito em nenhum componente do caminho): " +
                            quotePath(raw));
        }
        if (startsWith(raw, L"\\\\?\\") || startsWith(raw, L"\\\\.\\") || startsWith(raw, L"\\??\\"))
            return fail(WriteStage::PathCheck, SafeWriteError::UnsupportedPath, 0,
                        "caminhos com prefixo \\\\?\\, \\\\.\\ ou \\??\\ não são suportados: " +
                            quotePath(raw));
        {
            const std::string problem = fileNameProblem(input_.filename().native());
            if (!problem.empty())
                return fail(WriteStage::PathCheck,
                            input_.filename().empty() ? SafeWriteError::EmptyPath
                                                      : SafeWriteError::UnsupportedPath,
                            0, problem + ": " + quotePath(raw));
        }

        std::error_code ec;
        fs::path abs = fs::absolute(input_, ec);
        if (ec)
            return fail(WriteStage::PathCheck, SafeWriteError::UnsupportedPath,
                        static_cast<DWORD>(ec.value()),
                        "não foi possível converter o destino em caminho absoluto: " + quotePath(raw));
        abs = abs.lexically_normal();
        dest_ = abs.native();
        r_.destination = abs;
        {
            const std::string problem = fileNameProblem(abs.filename().native());
            if (!problem.empty())
                return fail(WriteStage::PathCheck, SafeWriteError::UnsupportedPath, 0,
                            problem + ": " + quotePath(dest_));
        }
        if (dest_.size() + kTempSuffixLength >= MAX_PATH)
            return fail(WriteStage::PathCheck, SafeWriteError::PathTooLong, 0,
                        "caminho longo demais: o destino tem " + std::to_string(dest_.size()) +
                            " caracteres e, com o nome do temporário (+" +
                            std::to_string(kTempSuffixLength) + "), ultrapassa o limite de " +
                            std::to_string(MAX_PATH - 1) +
                            " caracteres (MAX_PATH) suportado; escolha um caminho mais curto: " +
                            quotePath(dest_));

        const fs::path parent = abs.parent_path();
        dir_ = parent.native();
        const DWORD dirAttr = ::GetFileAttributesW(dir_.c_str());
        if (dirAttr == INVALID_FILE_ATTRIBUTES) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                return fail(WriteStage::PathCheck, SafeWriteError::DirectoryMissing, err,
                            "o diretório do destino não existe (não é criado automaticamente): " +
                                quotePath(dir_));
            return fail(WriteStage::PathCheck, SafeWriteError::DestinationInaccessible, err,
                        "não foi possível consultar o diretório do destino: " + quotePath(dir_));
        }
        if ((dirAttr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return fail(WriteStage::PathCheck, SafeWriteError::ParentNotDirectory, 0,
                        "o diretório do destino é um arquivo: " + quotePath(dir_));

        DWORD err = 0;
        const Presence presence = queryPath(dest_, destInfo_, err);
        if (presence == Presence::Error)
            return fail(WriteStage::PathCheck, SafeWriteError::DestinationInaccessible, err,
                        "não foi possível consultar o destino: " + quotePath(dest_));
        if (presence == Presence::Absent)
            return true;

        r_.destinationExisted = true;
        if (destInfo_.attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return fail(WriteStage::PathCheck, SafeWriteError::DestinationIsReparsePoint, 0,
                        "o destino é um link simbólico, junção ou outro reparse point e não é "
                        "substituído: " +
                            quotePath(dest_));
        if (destInfo_.attributes & FILE_ATTRIBUTE_DIRECTORY)
            return fail(WriteStage::PathCheck, SafeWriteError::DestinationIsDirectory, 0,
                        "o destino é um diretório: " + quotePath(dest_));
        if (destInfo_.attributes & FILE_ATTRIBUTE_READONLY)
            return fail(WriteStage::PathCheck, SafeWriteError::DestinationReadOnly, 0,
                        "o destino é somente leitura (o atributo não é removido automaticamente): " +
                            quotePath(dest_));
        if (options_.replace == ReplacePolicy::Forbid)
            return fail(WriteStage::PathCheck, SafeWriteError::DestinationExists, 0,
                        "o destino já existe e a substituição não foi autorizada: " + quotePath(dest_));
        return true;
    }

    // --- 3. temporário -----------------------------------------------------------------------
    bool createTemp()
    {
        r_.stage = WriteStage::CreateTemp;
        const std::wstring pid = hex(::GetCurrentProcessId(), 8);
        DWORD err = 0;
        for (int attempt = 0; attempt < kTempNameAttempts; ++attempt) {
            std::wstring candidate = dest_ + L"." + pid + L"-" + hex(randomComponent(), 16) + L".tmp";
            probe(FaultPoint::TempName, &dest_, nullptr, static_cast<std::size_t>(attempt), 0, &candidate);
            // CREATE_NEW: falha se o nome existir (nunca abre nem trunca arquivo preexistente).
            // Compartilhamento 0: nenhum outro handle enquanto o temporário é escrito.
            // FILE_ATTRIBUTE_NORMAL (não TEMPORARY): o atributo seria herdado pelo arquivo publicado.
            HANDLE h = INVALID_HANDLE_VALUE;
            if (!injectedFailure(probe(FaultPoint::CreateTemp, &dest_, &candidate)))
                h = ::CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                err = ::GetLastError();
                if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
                    continue; // colisão: outro nome
                return fail(WriteStage::CreateTemp, SafeWriteError::TempCreateFailed, err,
                            "não foi possível criar o arquivo temporário no diretório do destino " +
                                quotePath(dir_));
            }
            temp_.reset(h);
            if (::GetFileType(h) != FILE_TYPE_DISK) {
                // Não é um arquivo em disco (dispositivo): nada foi criado; nada é removido.
                temp_.close();
                return fail(WriteStage::CreateTemp, SafeWriteError::TempCreateFailed, 0,
                            "o nome do temporário não corresponde a um arquivo em disco: " +
                                quotePath(candidate));
            }
            tempCreated_ = true;
            // Identidade e caminho do temporário registrados ANTES de qualquer alocação (E5.7.4b.8c.4c):
            // se uma cópia de caminho abaixo lançar, a limpeza em internalFailure ainda conhece o arquivo
            // (caminho e volume/índice) e não afirma remoção sem evidência.
            const bool identified = infoOf(h, tempInfo_);
            const DWORD identifyError = identified ? 0 : ::GetLastError();
            tempPath_.swap(candidate); // não aloca
            r_.tempRemoved = false;
            r_.tempPath = fs::path(tempPath_);
            if (!identified) {
                const DWORD e = identifyError;
                abandonOpenTemp();
                fail(WriteStage::CreateTemp, SafeWriteError::TempCreateFailed, e,
                     "não foi possível identificar o arquivo temporário " + quotePath(tempPath_));
                appendCleanupNote();
                return false;
            }
            probe(FaultPoint::AfterCreateTemp, &dest_, &tempPath_);
            return true;
        }
        return fail(WriteStage::CreateTemp, SafeWriteError::TempCreateFailed, err,
                    "não foi possível obter um nome exclusivo para o temporário após " +
                        std::to_string(kTempNameAttempts) + " tentativas em " + quotePath(dir_));
    }

    // --- 4. escrita --------------------------------------------------------------------------
    bool writeTemp()
    {
        r_.stage = WriteStage::WriteTemp;
        const char* p = bytes_.data();
        std::size_t remaining = bytes_.size();
        std::size_t call = 0;
        while (remaining > 0) {
            if (cancelled())
                return failOpen(WriteStage::WriteTemp, SafeWriteError::Cancelled, 0,
                                "gravação cancelada durante a escrita do temporário");
            const DWORD chunk = static_cast<DWORD>(std::min(remaining, kSafeWriteChunkBytes));
            DWORD written = 0;
            const FaultDecision d = probe(FaultPoint::WriteChunk, &dest_, &tempPath_, call++);
            BOOL wrote = FALSE;
            if (injectedFailure(d))
                wrote = FALSE;
            else if (d.action == FaultAction::ZeroWrite)
                wrote = TRUE; // sucesso aparente sem bytes escritos
            else if (d.action == FaultAction::Partial && d.partialBytes > 0)
                wrote = ::WriteFile(temp_.get(), p, std::min<DWORD>(chunk, d.partialBytes), &written, nullptr);
            else
                wrote = ::WriteFile(temp_.get(), p, chunk, &written, nullptr);
            if (!wrote) {
                const DWORD err = ::GetLastError();
                return failOpen(WriteStage::WriteTemp, SafeWriteError::WriteFailed, err,
                                "falha ao escrever o temporário " + quotePath(tempPath_) + " após " +
                                    std::to_string(r_.bytes) + " bytes");
            }
            if (written == 0 || written > chunk)
                return failOpen(WriteStage::WriteTemp, SafeWriteError::WriteFailed, 0,
                                "WriteFile retornou " + std::to_string(written) + " bytes para um bloco de " +
                                    std::to_string(chunk) + " em " + quotePath(tempPath_));
            p += written;
            remaining -= written;
            r_.bytes += written;
        }
        if (r_.bytes != bytes_.size())
            return failOpen(WriteStage::WriteTemp, SafeWriteError::SizeMismatch, 0,
                            "bytes confirmados (" + std::to_string(r_.bytes) + ") diferentes do esperado (" +
                                std::to_string(bytes_.size()) + ")");
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(temp_.get(), &size)) {
            const DWORD err = ::GetLastError();
            return failOpen(WriteStage::WriteTemp, SafeWriteError::SizeMismatch, err,
                            "não foi possível obter o tamanho do temporário " + quotePath(tempPath_));
        }
        if (static_cast<std::uint64_t>(size.QuadPart) != static_cast<std::uint64_t>(bytes_.size()))
            return failOpen(WriteStage::WriteTemp, SafeWriteError::SizeMismatch, 0,
                            "tamanho do temporário (" + std::to_string(size.QuadPart) +
                                " bytes) diferente do esperado (" + std::to_string(bytes_.size()) + ")");
        return true;
    }

    // --- 5. flush e fechamento ---------------------------------------------------------------
    bool flushTemp()
    {
        r_.stage = WriteStage::FlushTemp;
        if (injectedFailure(probe(FaultPoint::Flush, &dest_, &tempPath_)) ||
            !::FlushFileBuffers(temp_.get())) {
            const DWORD err = ::GetLastError();
            return failOpen(WriteStage::FlushTemp, SafeWriteError::FlushFailed, err,
                            "FlushFileBuffers falhou no temporário " + quotePath(tempPath_));
        }
        const FaultDecision closeFault = probe(FaultPoint::Close, &dest_, &tempPath_);
        bool closed = temp_.close();
        if (closed && injectedFailure(closeFault))
            closed = false; // o handle real foi fechado; somente a falha de CloseHandle é simulada
        if (!closed) {
            const DWORD err = ::GetLastError();
            removeClosedTemp();
            fail(WriteStage::FlushTemp, SafeWriteError::CloseFailed, err,
                 "falha ao fechar o temporário " + quotePath(tempPath_));
            appendCleanupNote();
            return false;
        }
        return true;
    }

    // --- 6. verificação ----------------------------------------------------------------------
    bool verifyTemp()
    {
        if (!options_.verifyAfterWrite)
            return true;
        r_.stage = WriteStage::VerifyTemp;
        probe(FaultPoint::BeforeVerify, &dest_, &tempPath_);
        // Compartilhamento somente de leitura: ninguém escreve, renomeia ou exclui durante a releitura.
        Handle h;
        if (!injectedFailure(probe(FaultPoint::ReopenVerify, &dest_, &tempPath_)))
            h.reset(::CreateFileW(tempPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!h.valid()) {
            const DWORD err = ::GetLastError();
            return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, err,
                              "não foi possível reabrir o temporário para verificação " + quotePath(tempPath_));
        }
        FileInfo info;
        if (!infoOf(h.get(), info)) {
            const DWORD err = ::GetLastError();
            h.close();
            return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, err,
                              "não foi possível identificar o temporário reaberto " + quotePath(tempPath_));
        }
        if (!sameFile(info, tempInfo_)) {
            h.close();
            return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, 0,
                              "o temporário reaberto não é o arquivo criado por esta operação " +
                                  quotePath(tempPath_));
        }
        if (info.size != static_cast<ULONGLONG>(bytes_.size())) {
            h.close();
            return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyMismatch, 0,
                              "tamanho relido (" + std::to_string(info.size) + ") diferente do esperado (" +
                                  std::to_string(bytes_.size()) + ")");
        }

        std::vector<char> buffer(std::max<std::size_t>(1, std::min(bytes_.size(), kSafeWriteChunkBytes)));
        std::size_t offset = 0;
        std::size_t block = 0;
        while (offset < bytes_.size()) {
            const DWORD want = static_cast<DWORD>(std::min(bytes_.size() - offset, buffer.size()));
            DWORD got = 0;
            const BOOL read = ::ReadFile(h.get(), buffer.data(), want, &got, nullptr);
            const FaultDecision d = probe(FaultPoint::CompareChunk, &dest_, &tempPath_, block++);
            if (read && got > 0 && d.action == FaultAction::CorruptRead)
                buffer[0] = static_cast<char>(~buffer[0]);
            if (!read || injectedFailure(d)) {
                const DWORD err = ::GetLastError();
                h.close();
                return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, err,
                                  "falha ao reler o temporário na posição " + std::to_string(offset));
            }
            if (got == 0) {
                h.close();
                return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyMismatch, 0,
                                  "fim prematuro do temporário na posição " + std::to_string(offset));
            }
            if (std::memcmp(buffer.data(), bytes_.data() + offset, got) != 0) {
                std::size_t i = 0;
                while (i < got && buffer[i] == bytes_[offset + i])
                    ++i;
                h.close();
                return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyMismatch, 0,
                                  "conteúdo relido difere do gravado no byte " + std::to_string(offset + i));
            }
            offset += got;
        }
        DWORD extra = 0;
        if (!::ReadFile(h.get(), buffer.data(), 1, &extra, nullptr) || extra != 0) {
            const DWORD err = extra != 0 ? 0 : ::GetLastError();
            h.close();
            return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyMismatch, err,
                              "o temporário contém bytes além do esperado");
        }
        if (!h.close()) {
            const DWORD err = ::GetLastError();
            return failClosed(WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, err,
                              "falha ao fechar o temporário após a verificação");
        }
        verifiedInfo_ = info;
        verified_ = true;
        return true;
    }

    // --- 7. publicação -----------------------------------------------------------------------
    void publish()
    {
        r_.stage = WriteStage::Publish;
        probe(FaultPoint::BeforePublish, &dest_, &tempPath_);
        if (cancelled()) {
            failClosed(WriteStage::Publish, SafeWriteError::Cancelled, 0,
                       "gravação cancelada antes da publicação; o destino não foi alterado");
            return;
        }
        if (!tempUnchanged())
            return;
        DWORD flags = MOVEFILE_WRITE_THROUGH; // nunca MOVEFILE_COPY_ALLOWED
        if (options_.replace == ReplacePolicy::Allow)
            flags |= MOVEFILE_REPLACE_EXISTING;
        // Mensagem de sucesso montada ANTES da publicação (E5.7.4b.8c.4c): depois de uma MoveFileExW
        // bem-sucedida nenhuma operação aloca nem lança, de modo que o fato da publicação não pode ser
        // perdido por uma exceção posterior. Sem memória para montá-la, a publicação prossegue (todo o
        // trabalho já foi feito e verificado) e a mensagem será o texto curto de reserva.
        std::string doneMessage;
        try {
            doneMessage = std::string(r_.destinationExisted ? "arquivo substituído: " : "arquivo criado: ") +
                          quotePath(dest_) + " (" + std::to_string(r_.bytes) + " bytes)";
        } catch (...) {
            doneMessage.clear();
        }
        const bool moved = !injectedFailure(probe(FaultPoint::Publish, &dest_, &tempPath_, 0, flags)) &&
                           ::MoveFileExW(tempPath_.c_str(), dest_.c_str(), flags) != 0;
        if (moved) {
            // PONTO DE CONFIRMAÇÃO DA PUBLICAÇÃO. Daqui até o retorno somente atribuições de bool,
            // enumerações e inteiros e std::string::swap (nenhuma alocação, nenhuma exceção).
            published_ = true;
            r_.ok = true;
            r_.stage = WriteStage::Done;
            r_.error = SafeWriteError::None;
            r_.win32Error = 0;
            r_.tempRemoved = true; // o temporário passou a ser o destino
            r_.destinationReplaced = r_.destinationExisted;
            // Forbid: a MoveFileExW sem REPLACE_EXISTING só tem sucesso com o destino ausente.
            // Allow: um destino surgido após a verificação seria substituído sem que se possa saber.
            r_.destinationPreserved = options_.replace == ReplacePolicy::Forbid;
            if (!doneMessage.empty())
                r_.message.swap(doneMessage);
            else
                assignShort(r_.message, r_.destinationReplaced ? kShortReplaced : kShortCreated);
            return;
        }

        const DWORD err = ::GetLastError();
        const bool exists = err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS;

        // Confirmação do estado do destino antes de remover o temporário. Feita ANTES de montar qualquer
        // texto (E5.7.4b.8c.4c) e sem alocação: se a montagem da mensagem lançar, destinationPreserved já
        // reflete o que foi confirmado, nunca o valor presumido inicial.
        FileInfo tempNow;
        DWORD qerr = 0;
        const bool tempInPlace =
            queryPath(tempPath_, tempNow, qerr) == Presence::Exists && sameFile(tempNow, tempInfo_);
        bool preserved = tempInPlace;
        if (preserved && r_.destinationExisted) {
            FileInfo destNow;
            preserved = queryPath(dest_, destNow, qerr) == Presence::Exists && sameSnapshot(destNow, destInfo_);
        }
        r_.destinationPreserved = preserved;

        std::string why;
        if (exists && options_.replace == ReplacePolicy::Forbid)
            why = "o destino passou a existir antes da publicação e a substituição não foi autorizada";
        else if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION)
            why = "o destino pode estar aberto em outro programa (ANSYS, editor, antivírus) ou sem "
                  "permissão de escrita; feche-o e tente novamente";
        else if (err == ERROR_NOT_SAME_DEVICE)
            why = "o temporário e o destino estão em volumes diferentes (cópia entre volumes não é usada)";
        else
            why = "falha ao publicar o arquivo";

        failClosed(WriteStage::Publish, exists && options_.replace == ReplacePolicy::Forbid
                                            ? SafeWriteError::DestinationExists
                                            : SafeWriteError::PublishFailed,
                   err, why + ": " + quotePath(dest_));
        r_.destinationPreserved = preserved;
        if (!preserved)
            r_.message += "; não foi possível confirmar que o destino ficou inalterado";
    }

    // Conferência do temporário imediatamente antes da MoveFileExW: mesmo arquivo (volume e índice),
    // comum, com o tamanho esperado e, se verificado, a mesma data de modificação relida. Reduz a janela
    // entre o fechamento e a publicação; não a elimina (a MoveFileExW opera por caminho).
    bool tempUnchanged()
    {
        FileInfo now;
        DWORD qerr = 0;
        const Presence p = queryPath(tempPath_, now, qerr);
        std::string problem;
        if (p == Presence::Absent)
            problem = "o temporário não está mais no diretório";
        else if (p == Presence::Error)
            problem = "não foi possível consultar o temporário";
        else if (!sameFile(now, tempInfo_))
            problem = "o arquivo no caminho do temporário não é o criado por esta operação";
        else if (now.attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            problem = "o temporário deixou de ser um arquivo comum";
        else if (now.size != static_cast<ULONGLONG>(bytes_.size()))
            problem = "o tamanho do temporário mudou depois da verificação";
        else if (verified_ && (now.lastWrite.dwLowDateTime != verifiedInfo_.lastWrite.dwLowDateTime ||
                               now.lastWrite.dwHighDateTime != verifiedInfo_.lastWrite.dwHighDateTime))
            problem = "o temporário foi modificado depois da verificação";
        if (problem.empty())
            return true;
        failClosed(WriteStage::Publish, SafeWriteError::TempChanged, p == Presence::Error ? qerr : 0,
                   problem + " (" + quotePath(tempPath_) + "); nada foi publicado e o destino não foi alterado");
        return false;
    }

    // --- falhas com limpeza ------------------------------------------------------------------
    // Falha com o temporário ainda aberto (nosso handle exclusivo).
    bool failOpen(WriteStage stage, SafeWriteError error, DWORD win32, const std::string& message)
    {
        abandonOpenTemp();
        fail(stage, error, win32, message);
        appendCleanupNote();
        return false;
    }

    // Falha com o temporário já fechado.
    bool failClosed(WriteStage stage, SafeWriteError error, DWORD win32, const std::string& message)
    {
        removeClosedTemp();
        fail(stage, error, win32, message);
        appendCleanupNote();
        return false;
    }

    // Marca para exclusão pelo próprio handle (FileDispositionInfo) e fecha: sem janela de troca do
    // arquivo, porque o handle foi aberto com compartilhamento 0 pela própria operação.
    void abandonOpenTemp()
    {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        const bool marked = !injectedFailure(probe(FaultPoint::Cleanup, &dest_, &tempPath_)) &&
                            ::SetFileInformationByHandle(temp_.get(), FileDispositionInfo, &disposition,
                                                         sizeof disposition) != 0;
        const DWORD markErr = marked ? 0 : ::GetLastError();
        temp_.close();
        if (marked) {
            FileInfo now;
            DWORD qerr = 0;
            const Presence p = queryPath(tempPath_, now, qerr);
            r_.tempRemoved = !(p == Presence::Exists && sameFile(now, tempInfo_));
            if (!r_.tempRemoved)
                r_.cleanupWin32Error = qerr;
            return;
        }
        removeClosedTemp();
        if (!r_.tempRemoved && r_.cleanupWin32Error == 0)
            r_.cleanupWin32Error = markErr;
    }

    // Remove o temporário fechado somente se ele ainda for o arquivo criado por esta operação.
    void removeClosedTemp()
    {
        if (!tempCreated_ || published_)
            return;
        Handle h(::CreateFileW(tempPath_.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!h.valid()) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                r_.tempRemoved = true; // não há mais temporário desta operação no caminho
                return;
            }
            r_.tempRemoved = false;
            r_.cleanupWin32Error = err;
            return;
        }
        FileInfo info;
        if (!infoOf(h.get(), info)) {
            r_.tempRemoved = false;
            r_.cleanupWin32Error = ::GetLastError();
            return;
        }
        if (!sameFile(info, tempInfo_)) {
            // O arquivo no caminho não é o nosso: não é removido. O paradeiro do temporário original é
            // desconhecido, portanto não se afirma que ele foi removido.
            r_.tempRemoved = false;
            r_.tempPathForeign = true;
            return;
        }
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (injectedFailure(probe(FaultPoint::Cleanup, &dest_, &tempPath_)) ||
            !::SetFileInformationByHandle(h.get(), FileDispositionInfo, &disposition, sizeof disposition)) {
            r_.tempRemoved = false;
            r_.cleanupWin32Error = ::GetLastError();
            return;
        }
        h.close();
        r_.tempRemoved = true;
    }

    void appendCleanupNote()
    {
        if (!tempCreated_ || published_)
            return;
        if (r_.tempRemoved)
            r_.message += "; temporário removido";
        else if (r_.tempPathForeign)
            r_.message += "; ATENÇÃO: o arquivo em " + quotePath(tempPath_) +
                          " não é o temporário desta operação e não foi removido; o temporário original "
                          "não foi localizado";
        else
            r_.message += "; ATENÇÃO: o temporário " + quotePath(tempPath_) +
                          " não pôde ser removido e deve ser excluído manualmente" +
                          win32Text(r_.cleanupWin32Error);
    }

    const fs::path& input_;
    std::string_view bytes_;
    const SafeWriteOptions& options_;
    SafeWriteResult r_;

    std::wstring dest_;
    std::wstring dir_;
    std::wstring tempPath_;
    FileInfo destInfo_;
    FileInfo tempInfo_;
    FileInfo verifiedInfo_;
    bool verified_ = false;
    Handle temp_;
    bool tempCreated_ = false;
    bool published_ = false;
};

} // namespace

SafeWriteResult writeFileSafely(const std::filesystem::path& destination, std::string_view bytes,
                                const SafeWriteOptions& options)
{
    return Writer(destination, bytes, options).run();
}

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
void safe_write_testing::setFaultHook(FaultHook hook) { tlFaultHook = std::move(hook); }
#endif

const char* writeStageName(WriteStage stage)
{
    switch (stage) {
    case WriteStage::Validation: return "Validation";
    case WriteStage::PathCheck: return "PathCheck";
    case WriteStage::CreateTemp: return "CreateTemp";
    case WriteStage::WriteTemp: return "WriteTemp";
    case WriteStage::FlushTemp: return "FlushTemp";
    case WriteStage::VerifyTemp: return "VerifyTemp";
    case WriteStage::Publish: return "Publish";
    case WriteStage::Cleanup: return "Cleanup";
    case WriteStage::Done: return "Done";
    }
    return "?";
}

const char* safeWriteErrorName(SafeWriteError error)
{
    switch (error) {
    case SafeWriteError::None: return "None";
    case SafeWriteError::InvalidOptions: return "InvalidOptions";
    case SafeWriteError::Cancelled: return "Cancelled";
    case SafeWriteError::EmptyPath: return "EmptyPath";
    case SafeWriteError::UnsupportedPath: return "UnsupportedPath";
    case SafeWriteError::PathTooLong: return "PathTooLong";
    case SafeWriteError::DirectoryMissing: return "DirectoryMissing";
    case SafeWriteError::ParentNotDirectory: return "ParentNotDirectory";
    case SafeWriteError::DestinationIsDirectory: return "DestinationIsDirectory";
    case SafeWriteError::DestinationIsReparsePoint: return "DestinationIsReparsePoint";
    case SafeWriteError::DestinationReadOnly: return "DestinationReadOnly";
    case SafeWriteError::DestinationExists: return "DestinationExists";
    case SafeWriteError::DestinationInaccessible: return "DestinationInaccessible";
    case SafeWriteError::TempCreateFailed: return "TempCreateFailed";
    case SafeWriteError::WriteFailed: return "WriteFailed";
    case SafeWriteError::SizeMismatch: return "SizeMismatch";
    case SafeWriteError::FlushFailed: return "FlushFailed";
    case SafeWriteError::CloseFailed: return "CloseFailed";
    case SafeWriteError::VerifyFailed: return "VerifyFailed";
    case SafeWriteError::VerifyMismatch: return "VerifyMismatch";
    case SafeWriteError::PublishFailed: return "PublishFailed";
    case SafeWriteError::TempChanged: return "TempChanged";
    case SafeWriteError::Internal: return "Internal";
    }
    return "?";
}

} // namespace cad::discretization
