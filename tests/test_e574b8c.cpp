// E5.7.4b.8c.3 sem interface — testes de falha, cancelamento, concorrência e recuperação do gravador
// seguro Win32 (discretization/SafeFileWriter.h). composeDemDyna NÃO é usado.
//
// O mesmo arquivo gera dois executáveis (tests/CMakeLists.txt):
//  - test_e574b8c          compila SafeFileWriter.cpp com CAD_SAFE_WRITER_FAULT_INJECTION (gancho por
//                          thread nos pontos internos) e executa todos os grupos;
//  - test_e574b8c_producao liga a cad_discretization de produção (sem gancho) e executa somente os
//                          grupos que não dependem de injeção.
//
//  [F] falhas simuladas A–J (temporário, 1º bloco, bloco intermediário, escrita parcial, flush,
//      fechamento, reabertura, comparação, publicação, limpeza), cada uma com destino ausente/Forbid e
//      destino existente/Allow; o destino é conferido em todos os pontos internos (nunca truncado nem
//      removido antes da publicação);
//  [X] cancelamento antes do temporário, antes do 1º bloco, entre blocos, antes da publicação e depois
//      de iniciada a publicação (sem efeito);
//  [C] concorrência no mesmo destino com barreiras (sem sleeps): Forbid+Forbid, Allow+Allow,
//      Forbid+Allow (ordem livre e ordens forçadas) e destino criado por terceiro antes da publicação;
//  [P] caminhos e arquivos: Unicode, destino bloqueado, somente leitura, diretório inexistente, vazio,
//      binário, > 1 MiB, nomes reservados, caminho longo, prefixo \\?\, link simbólico e junção NTFS;
//  [T] identidade do temporário: substituição, modificação e remoção entre a verificação e a publicação;
//  [S] contrato: Forbid padrão, política inválida, CREATE_NEW diante de colisão de nome, flags da
//      MoveFileExW, destinationPreserved/destinationReplaced;
//  [N] NUL e caracteres de controle 0-31 em qualquer posição do caminho (E5.7.4b.8c.4d): recusa em
//      PathCheck/UnsupportedPath antes de fs::absolute, sentinela preservada (Allow), prefixo truncado
//      não criado (Forbid), diagnóstico escapado (\xNN) e caminhos válidos ainda aceitos; com injeção,
//      nenhum ponto interno (temporário, publicação) é alcançado;
//  [R] DEM_DYNA.txt de referência (somente leitura) gravado no diretório isolado: 58.181 bytes idênticos;
//  [M] contrato de publicação (E5.7.4b.8c.4c): casos A–C com todos os campos; falhas de alocação
//      injetadas em cada alocação do gravador (modo único e persistente; destino ausente/Forbid e
//      existente/Allow) — publicação efetiva ⇔ ok, stage = Done somente com ok; com injeção de falhas
//      de API (somente test_e574b8c): nenhuma alocação depois da MoveFileExW bem-sucedida (contagem e
//      falha armada no ponto da publicação), mensagem de sucesso preparada antes da publicação sem
//      memória, falha da MoveFileExW seguida de falta de memória (preservação confirmada, não
//      presumida) e cancelamento depois da publicação.
//
// Todas as escritas ocorrem num diretório exclusivo criado em %TEMP% para esta execução e removido no
// final (links e junções são removidos sem seguir o alvo). Coberturas que o ambiente não permite
// (link simbólico sem privilégio, junção) são registradas como NÃO EXECUTADAS, nunca como aprovadas.
#include "TestCheck.h"

#include "discretization/SafeFileWriter.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Injeção de falhas de alocação (E5.7.4b.8c.4c; somente neste executável de teste, grupo [M]). O
// operator new global é substituído; desarmado (padrão), apenas repassa a malloc. Armado, conta as
// alocações da thread atual e faz falhar a n-ésima (modo único) ou todas a partir dela (modo
// persistente, "pouca memória"); failAt = 0 apenas conta. Em Debug (_ITERATOR_DEBUG_LEVEL != 0) as
// alocações do tamanho de std::_Container_proxy não são contadas nem falham: o MSVC as faz dentro de
// construtores noexcept (falhar nelas encerraria o processo, não testaria o gravador). A injeção
// cobre somente falhas de operator new; não cobre todas as falhas possíveis (SEH, malloc de C, APIs).
namespace oom {
thread_local bool tArmed = false;
thread_local bool tPersistent = false;
thread_local long long tFailAt = 0; // 0 = apenas contar
thread_local long long tCount = 0;
thread_local long long tInjected = 0;

void arm(long long failAt, bool persistent)
{
    tCount = 0;
    tInjected = 0;
    tFailAt = failAt;
    tPersistent = persistent;
    tArmed = true;
}
long long disarm()
{
    tArmed = false;
    return tCount;
}
bool shouldFail(std::size_t n)
{
    if (!tArmed)
        return false;
#if defined(_MSC_VER) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    if (n == sizeof(std::_Container_proxy))
        return false;
#else
    (void)n;
#endif
    ++tCount;
    if (tFailAt > 0 && (tCount == tFailAt || (tPersistent && tCount > tFailAt))) {
        ++tInjected;
        return true;
    }
    return false;
}
} // namespace oom

void* operator new(std::size_t n)
{
    if (oom::shouldFail(n))
        throw std::bad_alloc();
    if (void* p = std::malloc(n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace cad::discretization;
namespace fs = std::filesystem;
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
namespace fi = cad::discretization::safe_write_testing;
#endif

namespace {

int g_checks = 0;
int g_scenarios = 0;
std::vector<std::string> g_notRun;

#define CHECK(c) (++g_checks, CAD_CHECK(c))

void scenario(const char* name)
{
    ++g_scenarios;
    std::printf("  - %s\n", name);
}

void notRun(const std::string& what)
{
    g_notRun.push_back(what);
    std::printf("  - NAO EXECUTADO: %s\n", what.c_str());
}

constexpr std::size_t kMiB = std::size_t{1} << 20;

// ---------------------------------------------------------------------------------------------
// Arquivos auxiliares (somente no diretório isolado).
std::optional<std::string> readFile(const fs::path& p)
{
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::nullopt;
    std::string out;
    char buf[65536];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
        out.append(buf, got);
    ::CloseHandle(h);
    return out;
}

bool writeRaw(const fs::path& p, const std::string& bytes)
{
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD w = 0;
    const bool ok = bytes.empty() ||
                    (::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr) && w == bytes.size());
    ::CloseHandle(h);
    return ok;
}

bool pathExists(const fs::path& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Arquivos "<nome do destino>.*.tmp" no diretório.
std::vector<std::wstring> tempsOf(const fs::path& dest)
{
    std::vector<std::wstring> out;
    const std::wstring prefix = dest.filename().native() + L".";
    WIN32_FIND_DATAW fd{};
    HANDLE f = ::FindFirstFileW((dest.parent_path() / L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n.size() > prefix.size() + 4 && n.compare(0, prefix.size(), prefix) == 0 &&
            n.compare(n.size() - 4, 4, L".tmp") == 0)
            out.push_back(n);
    } while (::FindNextFileW(f, &fd));
    ::FindClose(f);
    return out;
}

std::size_t entriesIn(const fs::path& dir)
{
    std::size_t n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE f = ::FindFirstFileW((dir / L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0)
            ++n;
    } while (::FindNextFileW(f, &fd));
    ::FindClose(f);
    return n;
}

// Conteúdo determinístico distinto por semente (sem períodos curtos: detecta mistura de blocos).
std::string pattern(std::size_t n, std::uint32_t seed)
{
    std::string s(n, '\0');
    std::uint32_t x = seed * 2654435761u + 12345u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[i] = static_cast<char>(x & 0xFF);
    }
    return s;
}

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// Remoção recursiva que NÃO segue reparse points (junções e links são removidos como entradas).
void removeTree(const fs::path& dir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE f = ::FindFirstFileW((dir / L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..")
                continue;
            const fs::path p = dir / n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                ::SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ::RemoveDirectoryW(p.c_str());
                else
                    ::DeleteFileW(p.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(p);
            } else {
                ::DeleteFileW(p.c_str());
            }
        } while (::FindNextFileW(f, &fd));
        ::FindClose(f);
    }
    ::RemoveDirectoryW(dir.c_str());
}

// Diretório exclusivo desta execução em %TEMP%.
class TestRoot
{
public:
    TestRoot()
    {
        wchar_t tmp[MAX_PATH + 1]{};
        const DWORD n = ::GetTempPathW(MAX_PATH + 1, tmp);
        fs::path base = n > 0 ? fs::path(tmp) : fs::path(L"C:\\Temp");
        for (int i = 0; i < 64 && root_.empty(); ++i) {
            wchar_t name[96];
            std::swprintf(name, 96, L"cad_e574b8c_%lx_%llx_%d", ::GetCurrentProcessId(),
                          static_cast<unsigned long long>(::GetTickCount64()), i);
            const fs::path candidate = base / name;
            if (::CreateDirectoryW(candidate.c_str(), nullptr))
                root_ = candidate;
        }
    }
    ~TestRoot()
    {
        if (!root_.empty() && contains(utf8(root_.filename().native()), "cad_e574b8c_"))
            removeTree(root_);
    }
    bool valid() const { return !root_.empty(); }
    const fs::path& path() const { return root_; }

    // Novo subdiretório exclusivo.
    fs::path sub(const std::string& tag)
    {
        const fs::path p = root_ / (std::to_wstring(++counter_) + L"_" + std::wstring(tag.begin(), tag.end()));
        ::CreateDirectoryW(p.c_str(), nullptr);
        return p;
    }

private:
    fs::path root_;
    int counter_ = 0;
};

TestRoot* g_root = nullptr;

// ---------------------------------------------------------------------------------------------
// Sincronização sem sleeps: barreira e evento (o tempo-limite só evita travar o CTest se uma thread
// não chegar ao ponto de encontro; não determina o resultado).
class Barrier
{
public:
    explicit Barrier(int n) : n_(n) {}
    bool arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(m_);
        const int gen = gen_;
        if (++count_ == n_) {
            count_ = 0;
            ++gen_;
            cv_.notify_all();
            return true;
        }
        return cv_.wait_for(lock, std::chrono::seconds(60), [&] { return gen_ != gen; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int n_;
    int count_ = 0;
    int gen_ = 0;
};

class Event
{
public:
    void set()
    {
        std::lock_guard<std::mutex> lock(m_);
        set_ = true;
        cv_.notify_all();
    }
    bool wait()
    {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, std::chrono::seconds(60), [&] { return set_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool set_ = false;
};

SafeWriteOptions opts(ReplacePolicy p, const std::atomic<bool>* cancel = nullptr, bool verify = true)
{
    SafeWriteOptions o;
    o.replace = p;
    o.cancel = cancel;
    o.verifyAfterWrite = verify;
    return o;
}

void printResult(const char* tag, const SafeWriteResult& r)
{
    std::printf("      %s ok=%d etapa=%s erro=%s win32=%lu bytes=%llu existia=%d substituido=%d "
                "preservado=%d tempRemovido=%d tempAlheio=%d limpezaWin32=%lu\n",
                tag, r.ok ? 1 : 0, writeStageName(r.stage), safeWriteErrorName(r.error),
                static_cast<unsigned long>(r.win32Error), static_cast<unsigned long long>(r.bytes),
                r.destinationExisted ? 1 : 0, r.destinationReplaced ? 1 : 0, r.destinationPreserved ? 1 : 0,
                r.tempRemoved ? 1 : 0, r.tempPathForeign ? 1 : 0, static_cast<unsigned long>(r.cleanupWin32Error));
}

// Sucesso com conteúdo idêntico e sem temporários.
void expectWritten(const SafeWriteResult& r, const fs::path& dest, const std::string& data)
{
    CHECK(r.ok);
    CHECK(r.stage == WriteStage::Done);
    CHECK(r.error == SafeWriteError::None);
    CHECK(r.bytes == data.size());
    CHECK(r.tempRemoved);
    const auto back = readFile(dest);
    CHECK(back.has_value() && *back == data);
    CHECK(tempsOf(dest).empty());
}

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
// Gancho da thread atual durante o escopo.
struct ScopedHook
{
    explicit ScopedHook(fi::FaultHook h) { fi::setFaultHook(std::move(h)); }
    ~ScopedHook() { fi::setFaultHook({}); }
};

fi::FaultDecision proceed() { return {}; }
fi::FaultDecision failWith(DWORD e)
{
    fi::FaultDecision d;
    d.action = fi::FaultAction::Fail;
    d.win32Error = e;
    return d;
}

const char* pointName(fi::FaultPoint p)
{
    switch (p) {
    case fi::FaultPoint::TempName: return "TempName";
    case fi::FaultPoint::CreateTemp: return "CreateTemp";
    case fi::FaultPoint::AfterCreateTemp: return "AfterCreateTemp";
    case fi::FaultPoint::WriteChunk: return "WriteChunk";
    case fi::FaultPoint::Flush: return "Flush";
    case fi::FaultPoint::Close: return "Close";
    case fi::FaultPoint::BeforeVerify: return "BeforeVerify";
    case fi::FaultPoint::ReopenVerify: return "ReopenVerify";
    case fi::FaultPoint::CompareChunk: return "CompareChunk";
    case fi::FaultPoint::BeforePublish: return "BeforePublish";
    case fi::FaultPoint::Publish: return "Publish";
    case fi::FaultPoint::Cleanup: return "Cleanup";
    }
    return "?";
}

// =============================================================================================
// [F] Matriz de falhas.
struct FaultCase
{
    const char* id;
    const char* name;
    std::function<fi::FaultDecision(const fi::FaultContext&)> decide;
    std::size_t size;
    WriteStage stage;
    SafeWriteError error;
    DWORD win32;                 // código esperado em r.win32Error
    std::uintmax_t bytes;        // bytes confirmados esperados (UINTMAX_MAX = não conferido)
    bool tempCreated;            // espera-se que um temporário tenha sido criado
    DWORD cleanupError;          // != 0: falha de limpeza intencional com este código
    const char* messagePart;     // trecho esperado na mensagem
};

struct MatrixRow
{
    std::string id;
    std::string variant;
    std::string stage;
    std::string error;
    unsigned long win32;
    bool okFalse;
    bool destOk;
    bool tempOk;
    bool diagOk;
};
std::vector<MatrixRow> g_matrix;

void runFaultCase(const FaultCase& fc, bool existing)
{
    const fs::path dir = g_root->sub(std::string(fc.id) + (existing ? "_allow" : "_forbid"));
    const fs::path dest = dir / L"saida.txt";
    const std::string original = pattern(7001, 4242);
    if (existing)
        CHECK(writeRaw(dest, original));
    const std::string data = pattern(fc.size, 17);

    // Monitor: em TODOS os pontos internos (antes, durante e depois da tentativa de publicação que
    // falha), o destino deve estar exatamente no estado inicial.
    int destChanged = 0;
    int probes = 0;
    auto monitor = [&](const fi::FaultContext& ctx) {
        ++probes;
        const auto now = readFile(dest);
        const bool same = existing ? (now.has_value() && *now == original) : !now.has_value();
        if (!same) {
            ++destChanged;
            std::printf("      destino alterado no ponto %s\n", pointName(ctx.point));
        }
        return fc.decide(ctx);
    };

    SafeWriteResult r;
    {
        ScopedHook hook(monitor);
        r = writeFileSafely(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid));
    }
    printResult(existing ? "[existente/Allow]" : "[ausente/Forbid] ", r);

    MatrixRow row{fc.id, existing ? "existente/Allow" : "ausente/Forbid", writeStageName(r.stage),
                  safeWriteErrorName(r.error), static_cast<unsigned long>(r.win32Error), false, false, false, false};

    CHECK(!r.ok);
    CHECK(r.stage == fc.stage);
    CHECK(r.error == fc.error);
    CHECK(r.win32Error == fc.win32);
    CHECK(!r.message.empty());
    CHECK(contains(r.message, fc.messagePart));
    if (fc.bytes != UINTMAX_MAX)
        CHECK(r.bytes == fc.bytes);
    CHECK(probes > 0);
    CHECK(destChanged == 0);
    CHECK(r.destinationExisted == existing);
    CHECK(!r.destinationReplaced);
    row.okFalse = !r.ok && r.stage == fc.stage && r.error == fc.error && r.win32Error == fc.win32;

    // Destino: preservação comprovada pelo conteúdo (o próprio teste confere o arquivo).
    const auto after = readFile(dest);
    const bool destOk = existing ? (after.has_value() && *after == original) : !after.has_value();
    CHECK(destOk);
    CHECK(r.destinationPreserved);
    row.destOk = destOk && r.destinationPreserved;

    const auto temps = tempsOf(dest);
    CHECK(r.tempPath.empty() == !fc.tempCreated);
    CHECK(!r.tempPathForeign);
    if (fc.cleanupError == 0) {
        CHECK(r.tempRemoved);
        CHECK(temps.empty());
        CHECK(r.cleanupWin32Error == 0);
        if (fc.tempCreated)
            CHECK(contains(r.message, "temporário removido"));
        row.tempOk = r.tempRemoved && temps.empty();
        row.diagOk = !contains(r.message, "ATENÇÃO");
    } else {
        // Limpeza intencionalmente falha: o temporário fica, o diagnóstico o indica e o resultado
        // distingue a falha principal (stage/error) da falha de limpeza.
        CHECK(!r.tempRemoved);
        CHECK(r.cleanupWin32Error == fc.cleanupError);
        CHECK(temps.size() == 1);
        CHECK(temps.size() == 1 && r.tempPath.filename().native() == temps[0]);
        CHECK(contains(r.message, "ATENÇÃO"));
        CHECK(contains(r.message, utf8(r.tempPath.filename().native())));
        CHECK(contains(r.message, "excluído manualmente"));
        row.tempOk = !r.tempRemoved && temps.size() == 1;
        row.diagOk = contains(r.message, "ATENÇÃO") && r.cleanupWin32Error == fc.cleanupError;
        // Remoção manual, como o diagnóstico instrui (somente no diretório de teste).
        for (const auto& t : temps)
            ::DeleteFileW((dir / t).c_str());
        CHECK(tempsOf(dest).empty());
    }
    g_matrix.push_back(row);
}

void testFaults()
{
    std::printf("[F] falhas simuladas\n");
    const std::size_t big = 2 * kMiB + 4321; // 3 blocos
    const DWORD diskFull = ERROR_DISK_FULL, io = ERROR_IO_DEVICE, denied = ERROR_ACCESS_DENIED,
                sharing = ERROR_SHARING_VIOLATION;

    std::vector<FaultCase> cases = {
        {"A", "criação do temporário (CreateFileW)",
         [=](const fi::FaultContext& c) { return c.point == fi::FaultPoint::CreateTemp ? failWith(denied) : proceed(); },
         1000, WriteStage::CreateTemp, SafeWriteError::TempCreateFailed, denied, 0, false, 0,
         "não foi possível criar o arquivo temporário"},
        {"A2", "colisão em todas as 16 tentativas de nome",
         [](const fi::FaultContext& c) {
             return c.point == fi::FaultPoint::CreateTemp ? failWith(ERROR_FILE_EXISTS) : proceed();
         },
         1000, WriteStage::CreateTemp, SafeWriteError::TempCreateFailed, ERROR_FILE_EXISTS, 0, false, 0,
         "após 16 tentativas"},
        {"B", "escrita do primeiro bloco",
         [=](const fi::FaultContext& c) {
             return c.point == fi::FaultPoint::WriteChunk && c.chunk == 0 ? failWith(diskFull) : proceed();
         },
         big, WriteStage::WriteTemp, SafeWriteError::WriteFailed, diskFull, 0, true, 0, "após 0 bytes"},
        {"C", "escrita de bloco intermediário",
         [=](const fi::FaultContext& c) {
             return c.point == fi::FaultPoint::WriteChunk && c.chunk == 1 ? failWith(diskFull) : proceed();
         },
         big, WriteStage::WriteTemp, SafeWriteError::WriteFailed, diskFull, kMiB, true, 0, "após 1048576 bytes"},
        {"D1", "escrita parcial seguida de WriteFile sem progresso",
         [](const fi::FaultContext& c) {
             fi::FaultDecision d;
             if (c.point == fi::FaultPoint::WriteChunk) {
                 d.action = c.chunk == 0 ? fi::FaultAction::Partial : fi::FaultAction::ZeroWrite;
                 d.partialBytes = 4096;
             }
             return d;
         },
         big, WriteStage::WriteTemp, SafeWriteError::WriteFailed, 0, 4096, true, 0, "WriteFile retornou 0 bytes"},
        {"D2", "escrita parcial seguida de erro",
         [=](const fi::FaultContext& c) {
             fi::FaultDecision d;
             if (c.point == fi::FaultPoint::WriteChunk) {
                 if (c.chunk == 0) {
                     d.action = fi::FaultAction::Partial;
                     d.partialBytes = 777;
                 } else {
                     d = failWith(diskFull);
                 }
             }
             return d;
         },
         big, WriteStage::WriteTemp, SafeWriteError::WriteFailed, diskFull, 777, true, 0, "após 777 bytes"},
        {"E", "FlushFileBuffers",
         [=](const fi::FaultContext& c) { return c.point == fi::FaultPoint::Flush ? failWith(io) : proceed(); },
         big, WriteStage::FlushTemp, SafeWriteError::FlushFailed, io, big, true, 0, "FlushFileBuffers falhou"},
        {"F", "fechamento do handle (CloseHandle)",
         [=](const fi::FaultContext& c) { return c.point == fi::FaultPoint::Close ? failWith(io) : proceed(); },
         big, WriteStage::FlushTemp, SafeWriteError::CloseFailed, io, big, true, 0, "falha ao fechar"},
        {"G", "reabertura para verificação",
         [=](const fi::FaultContext& c) {
             return c.point == fi::FaultPoint::ReopenVerify ? failWith(sharing) : proceed();
         },
         big, WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, sharing, big, true, 0, "reabrir o temporário"},
        {"H", "comparação de conteúdo (bloco relido divergente)",
         [](const fi::FaultContext& c) {
             fi::FaultDecision d;
             if (c.point == fi::FaultPoint::CompareChunk && c.chunk == 1)
                 d.action = fi::FaultAction::CorruptRead;
             return d;
         },
         big, WriteStage::VerifyTemp, SafeWriteError::VerifyMismatch, 0, big, true, 0, "no byte 1048576"},
        {"H2", "erro de leitura durante a comparação",
         [=](const fi::FaultContext& c) {
             return c.point == fi::FaultPoint::CompareChunk && c.chunk == 2 ? failWith(ERROR_CRC) : proceed();
         },
         big, WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, ERROR_CRC, big, true, 0, "falha ao reler"},
        {"I", "publicação (MoveFileExW)",
         [=](const fi::FaultContext& c) { return c.point == fi::FaultPoint::Publish ? failWith(denied) : proceed(); },
         big, WriteStage::Publish, SafeWriteError::PublishFailed, denied, big, true, 0, "aberto em outro programa"},
        {"J1", "limpeza falha após falha de escrita (temporário aberto)",
         [=](const fi::FaultContext& c) {
             if (c.point == fi::FaultPoint::WriteChunk && c.chunk == 1)
                 return failWith(diskFull);
             return c.point == fi::FaultPoint::Cleanup ? failWith(denied) : proceed();
         },
         big, WriteStage::WriteTemp, SafeWriteError::WriteFailed, diskFull, kMiB, true, denied, "após 1048576 bytes"},
        {"J2", "limpeza falha após falha de verificação (temporário fechado)",
         [=](const fi::FaultContext& c) {
             if (c.point == fi::FaultPoint::ReopenVerify)
                 return failWith(sharing);
             return c.point == fi::FaultPoint::Cleanup ? failWith(denied) : proceed();
         },
         big, WriteStage::VerifyTemp, SafeWriteError::VerifyFailed, sharing, big, true, denied, "reabrir"},
        {"J3", "limpeza falha após falha de publicação",
         [=](const fi::FaultContext& c) {
             if (c.point == fi::FaultPoint::Publish)
                 return failWith(sharing);
             return c.point == fi::FaultPoint::Cleanup ? failWith(denied) : proceed();
         },
         big, WriteStage::Publish, SafeWriteError::PublishFailed, sharing, big, true, denied, "aberto em outro programa"},
    };

    for (const FaultCase& fc : cases) {
        for (bool existing : {false, true}) {
            const std::string title = std::string(fc.id) + ". " + fc.name + (existing ? " [existente/Allow]" : " [ausente/Forbid]");
            scenario(title.c_str());
            runFaultCase(fc, existing);
        }
    }

    // J4: a marcação de exclusão pelo handle aberto falha, mas a remoção do temporário fechado (2ª via)
    // funciona: tempRemoved verdadeiro, sem aviso.
    {
        scenario("J4. limpeza: 1ª via (handle aberto) falha, 2ª via remove");
        const fs::path dir = g_root->sub("J4");
        const fs::path dest = dir / L"saida.txt";
        int cleanupCalls = 0;
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::WriteChunk && c.chunk == 0)
                    return failWith(ERROR_DISK_FULL);
                if (c.point == fi::FaultPoint::Cleanup && cleanupCalls++ == 0)
                    return failWith(ERROR_ACCESS_DENIED);
                return proceed();
            });
            r = writeFileSafely(dest, pattern(100, 1), opts(ReplacePolicy::Forbid));
        }
        printResult("", r);
        CHECK(!r.ok && r.error == SafeWriteError::WriteFailed && r.win32Error == ERROR_DISK_FULL);
        CHECK(cleanupCalls == 2);
        CHECK(r.tempRemoved && r.cleanupWin32Error == 0);
        CHECK(tempsOf(dest).empty() && !pathExists(dest));
    }

    // I2: publicação falha e o destino foi alterado por terceiro no mesmo instante: a preservação não
    // pode ser confirmada → destinationPreserved = false (não se afirma o que não se comprova).
    {
        scenario("I2. publicação falha com destino alterado por terceiro: preservação não afirmada");
        const fs::path dir = g_root->sub("I2");
        const fs::path dest = dir / L"saida.txt";
        CHECK(writeRaw(dest, "original"));
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::Publish) {
                    writeRaw(dest, "alterado por terceiro, com outro tamanho");
                    return failWith(ERROR_ACCESS_DENIED);
                }
                return proceed();
            });
            r = writeFileSafely(dest, pattern(5000, 3), opts(ReplacePolicy::Allow));
        }
        printResult("", r);
        CHECK(!r.ok && r.stage == WriteStage::Publish && r.error == SafeWriteError::PublishFailed);
        CHECK(!r.destinationPreserved);
        CHECK(contains(r.message, "não foi possível confirmar"));
        CHECK(r.tempRemoved && tempsOf(dest).empty());
    }

    // D3: escritas parciais repetidas (4 KiB por chamada) completam a gravação corretamente.
    {
        scenario("D3. escritas parciais repetidas completam o arquivo");
        const fs::path dir = g_root->sub("D3");
        const fs::path dest = dir / L"saida.txt";
        const std::string data = pattern(kMiB + 999, 8);
        std::size_t calls = 0;
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                fi::FaultDecision d;
                if (c.point == fi::FaultPoint::WriteChunk) {
                    ++calls;
                    d.action = fi::FaultAction::Partial;
                    d.partialBytes = 4096;
                }
                return d;
            });
            r = writeFileSafely(dest, data, opts(ReplacePolicy::Forbid));
        }
        expectWritten(r, dest, data);
        CHECK(calls == (data.size() + 4095) / 4096);
    }

    // G2/H3: alterações REAIS do temporário em disco entre o fechamento e a reabertura.
    {
        scenario("G2. temporário com byte extra antes da verificação (tamanho)");
        const fs::path dir = g_root->sub("G2");
        const fs::path dest = dir / L"saida.txt";
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::BeforeVerify) {
                    HANDLE h = ::CreateFileW(c.tempPath->c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    DWORD w = 0;
                    ::WriteFile(h, "X", 1, &w, nullptr);
                    ::CloseHandle(h);
                }
                return proceed();
            });
            r = writeFileSafely(dest, pattern(3000, 5), opts(ReplacePolicy::Forbid));
        }
        CHECK(!r.ok && r.stage == WriteStage::VerifyTemp && r.error == SafeWriteError::VerifyMismatch);
        CHECK(contains(r.message, "tamanho relido (3001)"));
        CHECK(r.tempRemoved && tempsOf(dest).empty() && !pathExists(dest));
    }
    {
        scenario("H3. byte do temporário alterado em disco antes da verificação (conteúdo)");
        const fs::path dir = g_root->sub("H3");
        const fs::path dest = dir / L"saida.txt";
        const std::string data = pattern(3 * kMiB, 6);
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::BeforeVerify) {
                    HANDLE h = ::CreateFileW(c.tempPath->c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    LARGE_INTEGER pos{};
                    pos.QuadPart = 2 * kMiB + 17;
                    ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
                    const char b = static_cast<char>(~data[2 * kMiB + 17]);
                    DWORD w = 0;
                    ::WriteFile(h, &b, 1, &w, nullptr);
                    ::CloseHandle(h);
                }
                return proceed();
            });
            r = writeFileSafely(dest, data, opts(ReplacePolicy::Forbid));
        }
        CHECK(!r.ok && r.stage == WriteStage::VerifyTemp && r.error == SafeWriteError::VerifyMismatch);
        CHECK(contains(r.message, "no byte " + std::to_string(2 * kMiB + 17)));
        CHECK(r.tempRemoved && tempsOf(dest).empty() && !pathExists(dest));
    }
}

// =============================================================================================
// [X] Cancelamento.
void testCancellation()
{
    std::printf("[X] cancelamento\n");
    const std::string original = pattern(2222, 77);
    const std::string data = pattern(2 * kMiB + 10, 78);

    struct CancelCase
    {
        const char* name;
        fi::FaultPoint point;
        std::size_t chunk;
        WriteStage stage;
        std::uintmax_t bytes;
        bool tempCreated;
    };
    const CancelCase cases[] = {
        {"antes do primeiro bloco (temporário já criado)", fi::FaultPoint::AfterCreateTemp, 0, WriteStage::WriteTemp, 0, true},
        {"entre blocos (após o 1º bloco)", fi::FaultPoint::WriteChunk, 0, WriteStage::WriteTemp, kMiB, true},
        {"depois da verificação, antes da publicação", fi::FaultPoint::BeforePublish, 0, WriteStage::Publish,
         2 * kMiB + 10, true},
    };
    for (const CancelCase& cc : cases) {
        for (bool existing : {false, true}) {
            scenario((std::string(cc.name) + (existing ? " [existente/Allow]" : " [ausente/Forbid]")).c_str());
            const fs::path dir = g_root->sub("cancel");
            const fs::path dest = dir / L"saida.txt";
            if (existing)
                CHECK(writeRaw(dest, original));
            std::atomic<bool> cancel{false};
            SafeWriteResult r;
            {
                ScopedHook hook([&](const fi::FaultContext& c) {
                    if (c.point == cc.point && c.chunk == cc.chunk)
                        cancel.store(true);
                    return proceed();
                });
                r = writeFileSafely(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid, &cancel));
            }
            printResult("", r);
            CHECK(!r.ok && r.error == SafeWriteError::Cancelled && r.stage == cc.stage);
            CHECK(r.bytes == cc.bytes);
            CHECK(r.destinationPreserved);
            const auto after = readFile(dest);
            CHECK(existing ? (after.has_value() && *after == original) : !after.has_value());
            CHECK(r.tempRemoved && tempsOf(dest).empty());
            CHECK(!r.tempPath.empty() == cc.tempCreated);
        }
    }

    // Depois de iniciada a publicação o cancelamento não tem efeito (e não há reversão): o pedido de
    // cancelamento feito no ponto da MoveFileExW é ignorado e a publicação conclui.
    {
        scenario("cancelamento solicitado já na publicação: sem efeito, sem reversão");
        const fs::path dir = g_root->sub("cancel_late");
        const fs::path dest = dir / L"saida.txt";
        CHECK(writeRaw(dest, original));
        std::atomic<bool> cancel{false};
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::Publish)
                    cancel.store(true);
                return proceed();
            });
            r = writeFileSafely(dest, data, opts(ReplacePolicy::Allow, &cancel));
        }
        CHECK(cancel.load());
        expectWritten(r, dest, data);
        CHECK(r.destinationReplaced && !r.destinationPreserved);
    }
}
#endif // CAD_SAFE_WRITER_FAULT_INJECTION

// Cancelamento antes da criação do temporário (independe de injeção).
void testCancelBeforeStart()
{
    std::printf("[X0] cancelamento antes de iniciar\n");
    for (bool existing : {false, true}) {
        scenario(existing ? "cancelado antes da criação do temporário [existente/Allow]"
                          : "cancelado antes da criação do temporário [ausente/Forbid]");
        const fs::path dir = g_root->sub("cancel0");
        const fs::path dest = dir / L"saida.txt";
        if (existing)
            CHECK(writeRaw(dest, "anterior"));
        std::atomic<bool> cancel{true};
        const SafeWriteResult r =
            writeFileSafely(dest, "novo", opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid, &cancel));
        CHECK(!r.ok && r.stage == WriteStage::Validation && r.error == SafeWriteError::Cancelled);
        CHECK(r.tempPath.empty() && r.tempRemoved && r.destinationPreserved);
        const auto after = readFile(dest);
        CHECK(existing ? (after.has_value() && *after == "anterior") : !after.has_value());
        CHECK(entriesIn(dir) == (existing ? 1u : 0u));
    }
}

// =============================================================================================
// [C] Concorrência.
struct Pair
{
    SafeWriteResult a, b;
};

bool isOneOf(const std::optional<std::string>& f, const std::string& x, const std::string& y)
{
    return f.has_value() && (*f == x || *f == y);
}

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
// order: 0 = livre (as duas threads se encontram antes da publicação e disputam a MoveFileExW);
//        1 = A publica e termina antes de B chamar a MoveFileExW; 2 = B antes de A.
Pair racePublish(const fs::path& dest, const std::string& da, ReplacePolicy pa, const std::string& db,
                 ReplacePolicy pb, int order, fi::FaultHook extraA = {})
{
    Pair out;
    Barrier meet(2);
    Event doneA, doneB;
    std::atomic<int> meetFailures{0};
    auto body = [&](bool isA) {
        Event& mine = isA ? doneA : doneB;
        Event& other = isA ? doneB : doneA;
        const bool second = (order == 1 && !isA) || (order == 2 && isA);
        fi::setFaultHook([&, isA, second](const fi::FaultContext& c) {
            if (c.point == fi::FaultPoint::BeforePublish && !meet.arriveAndWait())
                ++meetFailures;
            if (c.point == fi::FaultPoint::Publish && second && !other.wait())
                ++meetFailures;
            if (isA && extraA)
                return extraA(c);
            return proceed();
        });
        SafeWriteResult r = writeFileSafely(dest, isA ? da : db, opts(isA ? pa : pb));
        fi::setFaultHook({});
        (isA ? out.a : out.b) = std::move(r);
        mine.set();
    };
    std::thread ta(body, true), tb(body, false);
    ta.join();
    tb.join();
    CHECK(meetFailures.load() == 0);
    return out;
}
#else
// Sem injeção: as duas threads partem juntas de uma barreira (sem controle sobre a publicação).
Pair racePublish(const fs::path& dest, const std::string& da, ReplacePolicy pa, const std::string& db,
                 ReplacePolicy pb, int /*order*/)
{
    Pair out;
    Barrier start(2);
    std::thread ta([&] {
        start.arriveAndWait();
        out.a = writeFileSafely(dest, da, opts(pa));
    });
    std::thread tb([&] {
        start.arriveAndWait();
        out.b = writeFileSafely(dest, db, opts(pb));
    });
    ta.join();
    tb.join();
    return out;
}
#endif

void testConcurrency()
{
    std::printf("[C] concorrência no mesmo destino\n");
    const std::string da = pattern(3 * kMiB + 11, 1001);
    const std::string db = pattern(3 * kMiB + 11, 2002); // mesmo tamanho: mistura só por conteúdo
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    const int kFree = 12;
    const int orders[] = {0, 1, 2};
#else
    const int kFree = 12;
    const int orders[] = {0};
#endif

    // A. Forbid + Forbid
    {
        scenario("A. Forbid + Forbid (ordem livre e forçada)");
        int runs = 0, okTotal = 0;
        for (int order : orders) {
            for (int i = 0; i < (order == 0 ? kFree : 1); ++i, ++runs) {
                const fs::path dir = g_root->sub("cFF");
                const fs::path dest = dir / L"saida.txt";
                const Pair p = racePublish(dest, da, ReplacePolicy::Forbid, db, ReplacePolicy::Forbid, order);
                const int oks = (p.a.ok ? 1 : 0) + (p.b.ok ? 1 : 0);
                okTotal += oks;
                CHECK(oks == 1); // no máximo uma (e, com o destino ausente, exatamente uma)
                const SafeWriteResult& loser = p.a.ok ? p.b : p.a;
                CHECK(loser.error == SafeWriteError::DestinationExists);
                CHECK(loser.stage == WriteStage::Publish || loser.stage == WriteStage::PathCheck);
                CHECK(loser.tempRemoved);
                const auto f = readFile(dest);
                CHECK(f.has_value() && *f == (p.a.ok ? da : db)); // integral, sem mistura
                CHECK(tempsOf(dest).empty());
                if (order == 1)
                    CHECK(p.a.ok && !p.b.ok);
                if (order == 2)
                    CHECK(p.b.ok && !p.a.ok);
            }
        }
        std::printf("      %d execuções, %d publicações bem-sucedidas (1 por execução)\n", runs, okTotal);
    }

    // B. Allow + Allow
    {
        scenario("B. Allow + Allow (ordem livre e forçada)");
        int runs = 0, both = 0;
        for (int order : orders) {
            for (int i = 0; i < (order == 0 ? kFree : 1); ++i, ++runs) {
                const fs::path dir = g_root->sub("cAA");
                const fs::path dest = dir / L"saida.txt";
                const Pair p = racePublish(dest, da, ReplacePolicy::Allow, db, ReplacePolicy::Allow, order);
                CHECK(p.a.ok || p.b.ok);
                if (p.a.ok && p.b.ok)
                    ++both;
                for (const SafeWriteResult* r : {&p.a, &p.b})
                    if (!r->ok) {
                        CHECK(r->error == SafeWriteError::PublishFailed);
                        CHECK(r->tempRemoved);
                    }
                const auto f = readFile(dest);
                CHECK(isOneOf(f, da, db));
                CHECK(tempsOf(dest).empty());
                if (order == 1) // B publicou por último: substituiu A
                    CHECK(p.a.ok && p.b.ok && f.has_value() && *f == db);
                if (order == 2)
                    CHECK(p.a.ok && p.b.ok && f.has_value() && *f == da);
                if (!p.a.ok)
                    CHECK(f.has_value() && *f == db);
                if (!p.b.ok)
                    CHECK(f.has_value() && *f == da);
            }
        }
        std::printf("      %d execuções; em %d ambas publicaram (a última substituiu a primeira)\n", runs, both);
    }

    // C. Forbid (A) + Allow (B)
    {
        scenario("C. Forbid + Allow (resultado conforme a ordem efetiva)");
        int runs = 0, forbidFirst = 0, allowFirst = 0;
        for (int order : orders) {
            for (int i = 0; i < (order == 0 ? kFree : 1); ++i, ++runs) {
                const fs::path dir = g_root->sub("cFA");
                const fs::path dest = dir / L"saida.txt";
                const Pair p = racePublish(dest, da, ReplacePolicy::Forbid, db, ReplacePolicy::Allow, order);
                const auto f = readFile(dest);
                CHECK(isOneOf(f, da, db));
                CHECK(tempsOf(dest).empty());
                CHECK(p.a.ok || p.b.ok);
                if (p.a.ok && p.b.ok) {
                    // Forbid publicou primeiro (destino ausente) e Allow o substituiu depois.
                    ++forbidFirst;
                    CHECK(f.has_value() && *f == db);
                    CHECK(!p.b.destinationPreserved); // Allow nunca afirma preservação
                } else if (p.b.ok) {
                    // Allow primeiro: Forbid recusado na publicação ou já na verificação.
                    ++allowFirst;
                    CHECK(p.a.error == SafeWriteError::DestinationExists);
                    CHECK(f.has_value() && *f == db);
                    CHECK(p.a.tempRemoved);
                } else {
                    // Somente Forbid publicou: Allow falhou na MoveFileExW sem alterar nada.
                    CHECK(p.b.error == SafeWriteError::PublishFailed);
                    CHECK(f.has_value() && *f == da);
                }
                if (order == 1)
                    CHECK(p.a.ok && p.b.ok);
                if (order == 2)
                    CHECK(p.b.ok && !p.a.ok && p.a.stage == WriteStage::Publish);
            }
        }
        std::printf("      %d execuções: Forbid antes de Allow = %d, Allow antes de Forbid = %d\n", runs,
                    forbidFirst, allowFirst);
    }

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    // D. Destino criado por terceiro entre a verificação inicial e a publicação.
    for (ReplacePolicy pol : {ReplacePolicy::Forbid, ReplacePolicy::Allow}) {
        scenario(pol == ReplacePolicy::Forbid ? "D. destino criado por terceiro antes da publicação [Forbid]"
                                              : "D. destino criado por terceiro antes da publicação [Allow]");
        const fs::path dir = g_root->sub("cThird");
        const fs::path dest = dir / L"saida.txt";
        const std::string third = "arquivo criado por terceiro";
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::BeforePublish)
                    writeRaw(dest, third);
                return proceed();
            });
            r = writeFileSafely(dest, da, opts(pol));
        }
        printResult("", r);
        CHECK(!r.destinationExisted);
        const auto f = readFile(dest);
        if (pol == ReplacePolicy::Forbid) {
            CHECK(!r.ok && r.stage == WriteStage::Publish && r.error == SafeWriteError::DestinationExists);
            CHECK(f.has_value() && *f == third);
            CHECK(r.tempRemoved);
        } else {
            // Allow substitui (comportamento documentado) e não afirma preservação.
            CHECK(r.ok && f.has_value() && *f == da);
            CHECK(!r.destinationPreserved);
        }
        CHECK(tempsOf(dest).empty());
    }
#endif
}

// =============================================================================================
// [P] Caminhos e arquivos.
void testPaths()
{
    std::printf("[P] caminhos e arquivos\n");

    {
        scenario("Unicode no diretório e no nome do arquivo");
        const fs::path dir = g_root->path() / L"dir_ção_日本語_Ωμ";
        CHECK(::CreateDirectoryW(dir.c_str(), nullptr) != 0);
        const fs::path dest = dir / L"saída_ü_\U0001F642_результат.txt";
        const std::string data = "conteúdo UTF-8: ação, 日本, 🙂\r\n";
        const SafeWriteResult r = writeFileSafely(dest, data, opts(ReplacePolicy::Forbid));
        expectWritten(r, dest, data);
        CHECK(r.destination == dest);
        CHECK(contains(r.message, "saída_ü_"));
    }

    {
        const fs::path dir = g_root->sub("locked");
        const fs::path dest = dir / L"saida.txt";
        const std::string original = pattern(1234, 9);
        for (DWORD share : {DWORD{0}, DWORD{FILE_SHARE_READ}, DWORD{FILE_SHARE_READ | FILE_SHARE_WRITE}}) {
            scenario(share == 0 ? "destino aberto sem compartilhamento [Allow]"
                                : share == FILE_SHARE_READ ? "destino aberto com FILE_SHARE_READ [Allow]"
                                                           : "destino aberto com FILE_SHARE_READ|WRITE [Allow]");
            CHECK(writeRaw(dest, original));
            HANDLE lock = ::CreateFileW(dest.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
            CHECK(lock != INVALID_HANDLE_VALUE);
            const SafeWriteResult r = writeFileSafely(dest, pattern(5000, 10), opts(ReplacePolicy::Allow));
            const SafeWriteResult rf = writeFileSafely(dest, pattern(5000, 10), opts(ReplacePolicy::Forbid));
            ::CloseHandle(lock);
            printResult("", r);
            CHECK(!r.ok && r.stage == WriteStage::Publish && r.error == SafeWriteError::PublishFailed);
            CHECK(r.win32Error == ERROR_SHARING_VIOLATION || r.win32Error == ERROR_ACCESS_DENIED);
            CHECK(r.destinationPreserved); // confirmado: mesma identidade, tamanho, data e atributos
            CHECK(contains(r.message, "aberto em outro programa"));
            CHECK(r.tempRemoved);
            CHECK(!rf.ok && rf.stage == WriteStage::PathCheck && rf.error == SafeWriteError::DestinationExists);
            const auto after = readFile(dest);
            CHECK(after.has_value() && *after == original);
            CHECK(tempsOf(dest).empty());
        }
    }

    {
        scenario("destino somente leitura [Allow]");
        const fs::path dir = g_root->sub("readonly");
        const fs::path dest = dir / L"saida.txt";
        CHECK(writeRaw(dest, "protegido"));
        CHECK(::SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_READONLY) != 0);
        const SafeWriteResult r = writeFileSafely(dest, "novo", opts(ReplacePolicy::Allow));
        CHECK(!r.ok && r.stage == WriteStage::PathCheck && r.error == SafeWriteError::DestinationReadOnly);
        CHECK(::GetFileAttributesW(dest.c_str()) & FILE_ATTRIBUTE_READONLY); // atributo não removido
        const auto after = readFile(dest);
        CHECK(after.has_value() && *after == "protegido");
        CHECK(entriesIn(dir) == 1);
    }

    {
        scenario("diretório inexistente (não é criado)");
        const fs::path dir = g_root->path() / L"nao_existe";
        const SafeWriteResult r = writeFileSafely(dir / L"saida.txt", "x", opts(ReplacePolicy::Forbid));
        CHECK(!r.ok && r.stage == WriteStage::PathCheck && r.error == SafeWriteError::DirectoryMissing);
        CHECK(!pathExists(dir));
    }
    {
        scenario("\"diretório\" do destino é um arquivo");
        const fs::path dir = g_root->sub("parentfile");
        CHECK(writeRaw(dir / L"arquivo", "x"));
        const SafeWriteResult r = writeFileSafely(dir / L"arquivo" / L"saida.txt", "x", opts(ReplacePolicy::Allow));
        CHECK(!r.ok && r.error == SafeWriteError::ParentNotDirectory);
    }
    {
        scenario("destino é um diretório");
        const fs::path dir = g_root->sub("destdir");
        CHECK(::CreateDirectoryW((dir / L"saida.txt").c_str(), nullptr) != 0);
        const SafeWriteResult r = writeFileSafely(dir / L"saida.txt", "x", opts(ReplacePolicy::Allow));
        CHECK(!r.ok && r.error == SafeWriteError::DestinationIsDirectory);
        CHECK(entriesIn(dir) == 1);
    }

    {
        scenario("arquivo vazio (0 bytes)");
        const fs::path dir = g_root->sub("empty");
        const fs::path dest = dir / L"vazio.txt";
        expectWritten(writeFileSafely(dest, std::string(), opts(ReplacePolicy::Forbid)), dest, std::string());
        scenario("arquivo vazio substituindo arquivo não vazio [Allow]");
        expectWritten(writeFileSafely(dest, std::string(), opts(ReplacePolicy::Allow)), dest, std::string());
        CHECK(writeRaw(dest, "abc"));
        const SafeWriteResult r = writeFileSafely(dest, std::string(), opts(ReplacePolicy::Allow));
        expectWritten(r, dest, std::string());
        CHECK(r.destinationReplaced && !r.destinationPreserved);
    }
    {
        scenario("conteúdo binário (todos os 256 valores, \\0, \\r\\n, ^Z)");
        const fs::path dir = g_root->sub("binary");
        const fs::path dest = dir / L"dados.bin";
        std::string data;
        for (int rep = 0; rep < 64; ++rep)
            for (int v = 0; v < 256; ++v)
                data.push_back(static_cast<char>(v));
        data += std::string("\r\n\n\r\x1a\0\0", 7);
        expectWritten(writeFileSafely(dest, data, opts(ReplacePolicy::Forbid)), dest, data);
    }
    for (std::size_t n : {kMiB - 1, kMiB, kMiB + 1, 2 * kMiB, 5 * kMiB + 123}) {
        const std::string title = "arquivo de " + std::to_string(n) + " bytes";
        scenario(title.c_str());
        const fs::path dir = g_root->sub("size");
        const fs::path dest = dir / L"grande.bin";
        const std::string data = pattern(n, static_cast<std::uint32_t>(n));
        expectWritten(writeFileSafely(dest, data, opts(ReplacePolicy::Forbid)), dest, data);
    }

    {
        scenario("nomes reservados e inválidos (nada é criado)");
        const fs::path dir = g_root->sub("reserved");
        const wchar_t* names[] = {L"CON",       L"con.txt", L"PRN.inp",  L"AUX",       L"NUL.dat",  L"nul",
                                  L"COM1",      L"com9.txt", L"LPT1.txt", L"LPT¹.txt",  L"COM²",     L"CONIN$",
                                  L"CONOUT$.x", L"Con .txt", L"a.txt:fluxo", L"a<b.txt", L"a|b.txt", L"a?.txt",
                                  L"a*.txt",    L"final.",   L"final ",   L"..",        L"."};
        int refused = 0;
        for (const wchar_t* n : names) {
            const SafeWriteResult r = writeFileSafely(dir / n, "x", opts(ReplacePolicy::Allow));
            const bool ok = !r.ok && r.stage == WriteStage::PathCheck &&
                            (r.error == SafeWriteError::UnsupportedPath || r.error == SafeWriteError::EmptyPath);
            if (!ok)
                std::printf("      nome aceito indevidamente: %s (%s)\n", utf8(n).c_str(), safeWriteErrorName(r.error));
            refused += ok ? 1 : 0;
            CHECK(ok);
            CHECK(r.tempPath.empty());
        }
        CHECK(entriesIn(dir) == 0);
        std::printf("      %d de %zu nomes recusados\n", refused, sizeof names / sizeof names[0]);
        // Nomes parecidos mas válidos.
        for (const wchar_t* n : {L"CONSOLE.txt", L"COM10.txt", L"LPT.txt", L"nulo.txt"}) {
            const SafeWriteResult r = writeFileSafely(dir / n, "ok", opts(ReplacePolicy::Forbid));
            CHECK(r.ok);
        }
        CHECK(entriesIn(dir) == 4);
    }

    {
        scenario("caminho longo: limite exato aceito, um caractere a mais recusado");
        const std::wstring base = g_root->path().native();
        const std::size_t dirLen = 200;
        if (base.size() + 2 >= dirLen) {
            notRun("caminho longo (diretório temporário base longo demais)");
        } else {
            const fs::path dir = g_root->path() / std::wstring(dirLen - base.size() - 1, L'd');
            CHECK(dir.native().size() == dirLen);
            CHECK(::CreateDirectoryW(dir.c_str(), nullptr) != 0);
            // destino + 30 (sufixo do temporário) deve ficar <= MAX_PATH - 1 = 259.
            const std::size_t maxDest = MAX_PATH - 1 - 30; // 229
            const std::wstring okName = std::wstring(maxDest - dirLen - 1 - 4, L'f') + L".txt";
            const fs::path okDest = dir / okName;
            CHECK(okDest.native().size() == maxDest);
            const SafeWriteResult r1 = writeFileSafely(okDest, "limite", opts(ReplacePolicy::Forbid));
            expectWritten(r1, okDest, "limite");
            const fs::path longDest = dir / (L"g" + okName);
            const SafeWriteResult r2 = writeFileSafely(longDest, "longo", opts(ReplacePolicy::Forbid));
            CHECK(!r2.ok && r2.stage == WriteStage::PathCheck && r2.error == SafeWriteError::PathTooLong);
            CHECK(!pathExists(longDest));
            const fs::path veryLong = dir / (std::wstring(240, L'h') + L".txt");
            CHECK(writeFileSafely(veryLong, "x", opts(ReplacePolicy::Forbid)).error == SafeWriteError::PathTooLong);
        }
        scenario("prefixos \\\\?\\ e \\\\.\\ recusados");
        const fs::path dir = g_root->sub("prefix");
        const SafeWriteResult r = writeFileSafely(fs::path(L"\\\\?\\" + (dir / L"a.txt").native()), "x",
                                                  opts(ReplacePolicy::Forbid));
        CHECK(!r.ok && r.error == SafeWriteError::UnsupportedPath);
        CHECK(writeFileSafely(fs::path(L"\\\\.\\COM1"), "x", opts(ReplacePolicy::Forbid)).error ==
              SafeWriteError::UnsupportedPath);
        CHECK(entriesIn(dir) == 0);
    }
}

// Link simbólico e junção NTFS (somente dentro do diretório isolado).
#pragma pack(push, 1)
struct MountPointHeader
{
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
};
#pragma pack(pop)

bool createJunction(const fs::path& link, const fs::path& target, DWORD& err)
{
    err = 0;
    if (!::CreateDirectoryW(link.c_str(), nullptr)) {
        err = ::GetLastError();
        return false;
    }
    const std::wstring substitute = L"\\??\\" + target.native();
    const std::wstring print = target.native();
    const std::size_t pathBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    std::vector<char> buf(sizeof(MountPointHeader) + pathBytes, 0);
    auto* h = reinterpret_cast<MountPointHeader*>(buf.data());
    h->tag = IO_REPARSE_TAG_MOUNT_POINT;
    h->dataLength = static_cast<WORD>(8 + pathBytes);
    h->substituteOffset = 0;
    h->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    h->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    h->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    wchar_t* paths = reinterpret_cast<wchar_t*>(buf.data() + sizeof(MountPointHeader));
    std::memcpy(paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(paths + substitute.size() + 1, print.c_str(), (print.size() + 1) * sizeof(wchar_t));

    HANDLE d = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (d == INVALID_HANDLE_VALUE) {
        err = ::GetLastError();
        ::RemoveDirectoryW(link.c_str());
        return false;
    }
    DWORD ret = 0;
    const BOOL ok = ::DeviceIoControl(d, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr,
                                      0, &ret, nullptr);
    if (!ok)
        err = ::GetLastError();
    ::CloseHandle(d);
    if (!ok)
        ::RemoveDirectoryW(link.c_str());
    return ok != 0;
}

void testLinks()
{
    std::printf("[P2] links simbólicos e reparse points\n");
    // Link simbólico de arquivo como destino.
    {
        const fs::path dir = g_root->sub("symlink");
        const fs::path target = dir / L"alvo.txt";
        const fs::path link = dir / L"link.txt";
        CHECK(writeRaw(target, "alvo original"));
        if (!::CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x2 /*ALLOW_UNPRIVILEGED_CREATE*/)) {
            notRun("link simbólico de arquivo como destino (CreateSymbolicLinkW falhou, Win32 " +
                   std::to_string(::GetLastError()) + ": sem privilégio/modo desenvolvedor)");
        } else {
            scenario("destino é link simbólico de arquivo [Allow e Forbid]");
            for (ReplacePolicy pol : {ReplacePolicy::Allow, ReplacePolicy::Forbid}) {
                const SafeWriteResult r = writeFileSafely(link, "novo", opts(pol));
                CHECK(!r.ok && r.stage == WriteStage::PathCheck && r.error == SafeWriteError::DestinationIsReparsePoint);
            }
            CHECK(::GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
            const auto t = readFile(target);
            CHECK(t.has_value() && *t == "alvo original");
            CHECK(tempsOf(link).empty());
        }
    }
    // Junção NTFS.
    {
        const fs::path dir = g_root->sub("junction");
        const fs::path target = dir / L"alvo_dir";
        const fs::path junction = dir / L"juncao";
        CHECK(::CreateDirectoryW(target.c_str(), nullptr) != 0);
        CHECK(writeRaw(target / L"existente.txt", "conteúdo do alvo"));
        DWORD err = 0;
        if (!createJunction(junction, target, err)) {
            notRun("junção NTFS (FSCTL_SET_REPARSE_POINT falhou, Win32 " + std::to_string(err) + ")");
        } else {
            scenario("destino é a própria junção NTFS (recusado)");
            for (ReplacePolicy pol : {ReplacePolicy::Allow, ReplacePolicy::Forbid}) {
                const SafeWriteResult r = writeFileSafely(junction, "novo", opts(pol));
                CHECK(!r.ok && r.stage == WriteStage::PathCheck && r.error == SafeWriteError::DestinationIsReparsePoint);
            }
            CHECK(::GetFileAttributesW(junction.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
            CHECK(entriesIn(target) == 1);

            scenario("junção no caminho do diretório (seguida, como documentado)");
            const SafeWriteResult r = writeFileSafely(junction / L"via_juncao.txt", "via junção", opts(ReplacePolicy::Forbid));
            CHECK(r.ok);
            const auto viaTarget = readFile(target / L"via_juncao.txt");
            CHECK(viaTarget.has_value() && *viaTarget == "via junção");
            CHECK(tempsOf(target / L"via_juncao.txt").empty());

            scenario("arquivo existente atrás da junção: Forbid recusa, conteúdo preservado");
            const SafeWriteResult rf = writeFileSafely(junction / L"existente.txt", "x", opts(ReplacePolicy::Forbid));
            CHECK(!rf.ok && rf.error == SafeWriteError::DestinationExists);
            const auto kept = readFile(target / L"existente.txt");
            CHECK(kept.has_value() && *kept == "conteúdo do alvo");

            // Remoção da junção sem seguir o alvo; o alvo permanece.
            CHECK(::RemoveDirectoryW(junction.c_str()) != 0);
            CHECK(pathExists(target / L"existente.txt"));
        }
    }
}

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
// =============================================================================================
// [T] Identidade do temporário entre o fechamento e a publicação.
void testTempIdentity()
{
    std::printf("[T] identidade do temporário antes da MoveFileExW\n");
    const std::string data = pattern(kMiB + 500, 55);
    const std::string original = "destino original";

    for (bool verify : {true, false}) {
        for (bool existing : {false, true}) {
            const std::string title = std::string("temporário substituído por arquivo de terceiro") +
                                      (verify ? "" : " (sem verificação)") +
                                      (existing ? " [existente/Allow]" : " [ausente/Forbid]");
            scenario(title.c_str());
            const fs::path dir = g_root->sub("swap");
            const fs::path dest = dir / L"saida.txt";
            if (existing)
                CHECK(writeRaw(dest, original));
            fs::path moved;
            const std::string foreign = "ARQUIVO DE TERCEIRO NO LUGAR DO TEMPORARIO";
            SafeWriteResult r;
            {
                ScopedHook hook([&](const fi::FaultContext& c) {
                    if (c.point == fi::FaultPoint::BeforePublish) {
                        moved = fs::path(*c.tempPath + L".movido");
                        ::MoveFileW(c.tempPath->c_str(), moved.c_str());
                        writeRaw(*c.tempPath, foreign);
                    }
                    return proceed();
                });
                r = writeFileSafely(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid, nullptr, verify));
            }
            printResult("", r);
            CHECK(!r.ok && r.stage == WriteStage::Publish && r.error == SafeWriteError::TempChanged);
            CHECK(contains(r.message, "não é o criado por esta operação"));
            CHECK(r.destinationPreserved);
            const auto after = readFile(dest);
            CHECK(existing ? (after.has_value() && *after == original) : !after.has_value());
            // O arquivo de terceiro não foi removido nem publicado; o resultado não afirma remoção.
            const auto f = readFile(r.tempPath);
            CHECK(f.has_value() && *f == foreign);
            CHECK(r.tempPathForeign && !r.tempRemoved);
            CHECK(contains(r.message, "não é o temporário desta operação e não foi removido"));
            // Limpeza manual pelo teste (o original movido e o arquivo de terceiro).
            ::DeleteFileW(moved.c_str());
            ::DeleteFileW(r.tempPath.c_str());
            CHECK(tempsOf(dest).empty());
        }
    }

    {
        scenario("temporário com tamanho alterado depois da verificação (mesma identidade)");
        const fs::path dir = g_root->sub("grow");
        const fs::path dest = dir / L"saida.txt";
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::BeforePublish) {
                    HANDLE h = ::CreateFileW(c.tempPath->c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    DWORD w = 0;
                    ::WriteFile(h, "!", 1, &w, nullptr);
                    ::CloseHandle(h);
                }
                return proceed();
            });
            r = writeFileSafely(dest, data, opts(ReplacePolicy::Forbid));
        }
        CHECK(!r.ok && r.error == SafeWriteError::TempChanged && contains(r.message, "tamanho"));
        CHECK(!pathExists(dest));
        CHECK(r.tempRemoved && !r.tempPathForeign && tempsOf(dest).empty()); // era o nosso: removido
    }
    {
        scenario("temporário com data de modificação alterada depois da verificação");
        const fs::path dir = g_root->sub("mtime");
        const fs::path dest = dir / L"saida.txt";
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::BeforePublish) {
                    HANDLE h = ::CreateFileW(c.tempPath->c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr,
                                             OPEN_EXISTING, 0, nullptr);
                    const char b = static_cast<char>(~data[10]);
                    LARGE_INTEGER pos{};
                    pos.QuadPart = 10;
                    ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
                    DWORD w = 0;
                    ::WriteFile(h, &b, 1, &w, nullptr);
                    FILETIME ft{};
                    ::GetFileTime(h, nullptr, nullptr, &ft);
                    ULARGE_INTEGER u{};
                    u.LowPart = ft.dwLowDateTime;
                    u.HighPart = ft.dwHighDateTime;
                    u.QuadPart += 10000000ull; // +1 s: independe da resolução do relógio
                    ft.dwLowDateTime = u.LowPart;
                    ft.dwHighDateTime = u.HighPart;
                    ::SetFileTime(h, nullptr, nullptr, &ft);
                    ::CloseHandle(h);
                }
                return proceed();
            });
            r = writeFileSafely(dest, data, opts(ReplacePolicy::Forbid));
        }
        CHECK(!r.ok && r.error == SafeWriteError::TempChanged && contains(r.message, "modificado"));
        CHECK(!pathExists(dest) && r.tempRemoved && tempsOf(dest).empty());
    }
    {
        scenario("temporário removido por terceiro antes da publicação");
        const fs::path dir = g_root->sub("gone");
        const fs::path dest = dir / L"saida.txt";
        CHECK(writeRaw(dest, original));
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::BeforePublish)
                    ::DeleteFileW(c.tempPath->c_str());
                return proceed();
            });
            r = writeFileSafely(dest, data, opts(ReplacePolicy::Allow));
        }
        CHECK(!r.ok && r.error == SafeWriteError::TempChanged && contains(r.message, "não está mais"));
        const auto after = readFile(dest);
        CHECK(after.has_value() && *after == original);
        CHECK(tempsOf(dest).empty());
    }
}

// =============================================================================================
// [S] Contrato de segurança (comportamental, observado nos pontos internos).
void testContract()
{
    std::printf("[S] contrato de segurança\n");
    {
        scenario("Forbid é o padrão; política inválida recusada");
        CHECK(SafeWriteOptions{}.replace == ReplacePolicy::Forbid);
        CHECK(SafeWriteOptions{}.verifyAfterWrite);
        const fs::path dir = g_root->sub("policy");
        const SafeWriteResult r = writeFileSafely(dir / L"a.txt", "x", opts(static_cast<ReplacePolicy>(7)));
        CHECK(!r.ok && r.stage == WriteStage::Validation && r.error == SafeWriteError::InvalidOptions);
        CHECK(entriesIn(dir) == 0);
    }
    {
        scenario("flags da MoveFileExW: WRITE_THROUGH sempre, REPLACE_EXISTING só com Allow, nunca COPY_ALLOWED");
        for (ReplacePolicy pol : {ReplacePolicy::Forbid, ReplacePolicy::Allow}) {
            const fs::path dir = g_root->sub("flags");
            const fs::path dest = dir / L"saida.txt";
            if (pol == ReplacePolicy::Allow)
                CHECK(writeRaw(dest, "anterior"));
            std::uint32_t flags = 0xFFFFFFFFu;
            bool destIntactAtPublish = false;
            SafeWriteResult r;
            {
                ScopedHook hook([&](const fi::FaultContext& c) {
                    if (c.point == fi::FaultPoint::Publish) {
                        flags = c.moveFlags;
                        // Imediatamente antes da renomeação o destino ainda existe intacto (não há
                        // exclusão prévia do destino seguida de renomeação).
                        const auto now = readFile(dest);
                        destIntactAtPublish = pol == ReplacePolicy::Allow ? (now.has_value() && *now == "anterior")
                                                                          : !now.has_value();
                    }
                    return proceed();
                });
                r = writeFileSafely(dest, "novo", opts(pol));
            }
            CHECK(r.ok);
            CHECK((flags & MOVEFILE_COPY_ALLOWED) == 0);
            CHECK((flags & MOVEFILE_WRITE_THROUGH) != 0);
            CHECK(((flags & MOVEFILE_REPLACE_EXISTING) != 0) == (pol == ReplacePolicy::Allow));
            CHECK(destIntactAtPublish);
            if (pol == ReplacePolicy::Allow)
                CHECK(r.destinationExisted && r.destinationReplaced && !r.destinationPreserved);
            else
                CHECK(!r.destinationExisted && !r.destinationReplaced && r.destinationPreserved);
        }
    }
    {
        scenario("CREATE_NEW: colisão com arquivo preexistente não o abre nem trunca");
        const fs::path dir = g_root->sub("collision");
        const fs::path dest = dir / L"saida.txt";
        const fs::path squat = dir / L"saida.txt.ocupado.tmp";
        CHECK(writeRaw(squat, "PREEXISTENTE"));
        int attempts = 0;
        SafeWriteResult r;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::TempName) {
                    ++attempts;
                    if (c.chunk == 0)
                        *c.candidate = squat.native();
                }
                return proceed();
            });
            r = writeFileSafely(dest, "conteúdo", opts(ReplacePolicy::Forbid));
        }
        CHECK(r.ok && attempts == 2 && r.tempPath.empty() == false && r.tempPath != squat);
        const auto s = readFile(squat);
        CHECK(s.has_value() && *s == "PREEXISTENTE");
        const auto d = readFile(dest);
        CHECK(d.has_value() && *d == "conteúdo");

        scenario("CREATE_NEW: 16 colisões → falha, arquivo preexistente intacto");
        const fs::path dest2 = dir / L"outro.txt";
        attempts = 0;
        {
            ScopedHook hook([&](const fi::FaultContext& c) {
                if (c.point == fi::FaultPoint::TempName) {
                    ++attempts;
                    *c.candidate = squat.native();
                }
                return proceed();
            });
            r = writeFileSafely(dest2, "x", opts(ReplacePolicy::Forbid));
        }
        CHECK(!r.ok && r.stage == WriteStage::CreateTemp && r.error == SafeWriteError::TempCreateFailed);
        CHECK(r.win32Error == ERROR_FILE_EXISTS && attempts == 16);
        CHECK(r.tempPath.empty() && r.tempRemoved);
        const auto s2 = readFile(squat);
        CHECK(s2.has_value() && *s2 == "PREEXISTENTE");
        CHECK(!pathExists(dest2));
        ::DeleteFileW(squat.c_str());
    }
}
#endif // CAD_SAFE_WRITER_FAULT_INJECTION

// =============================================================================================
// [N] NUL e caracteres de controle em qualquer posição do caminho (E5.7.4b.8c.4d, achados A1/A2/A6).
// Executado nos dois executáveis (produção e injeção). Todos os caminhos são montados com
// std::wstring de comprimento explícito e ficam dentro do diretório isolado; os casos relativos
// usam o diretório isolado como diretório atual (restaurado em seguida), de modo que nem uma
// implementação defeituosa (truncamento no NUL) escreveria fora dele.

// Entradas do diretório, recursivamente, sem seguir reparse points ("sub\\nome").
void listTree(const fs::path& dir, const std::wstring& prefix, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd{};
    HANDLE f = ::FindFirstFileW((dir / L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..")
            continue;
        out.push_back(prefix + n);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            listTree(dir / n, prefix + n + L"\\", out);
    } while (::FindNextFileW(f, &fd));
    ::FindClose(f);
}
std::vector<std::wstring> tree(const fs::path& dir)
{
    std::vector<std::wstring> out;
    listTree(dir, L"", out);
    return out;
}

// Identidade (volume, índice NTFS) e data de modificação de um arquivo.
struct FileIdentity
{
    DWORD volume = 0, indexHigh = 0, indexLow = 0;
    FILETIME lastWrite{};
    bool operator==(const FileIdentity& o) const
    {
        return volume == o.volume && indexHigh == o.indexHigh && indexLow == o.indexLow &&
               lastWrite.dwLowDateTime == o.lastWrite.dwLowDateTime &&
               lastWrite.dwHighDateTime == o.lastWrite.dwHighDateTime;
    }
};
std::optional<FileIdentity> identityOf(const fs::path& p)
{
    HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::nullopt;
    BY_HANDLE_FILE_INFORMATION bi{};
    const bool ok = ::GetFileInformationByHandle(h, &bi) != 0;
    ::CloseHandle(h);
    if (!ok)
        return std::nullopt;
    FileIdentity id;
    id.volume = bi.dwVolumeSerialNumber;
    id.indexHigh = bi.nFileIndexHigh;
    id.indexLow = bi.nFileIndexLow;
    id.lastWrite = bi.ftLastWriteTime;
    return id;
}

// Caractere com comprimento explícito (L'\0' incluído).
std::wstring wch(wchar_t c) { return std::wstring(1, c); }

std::string escaped(unsigned code)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "\\x%02X", code);
    return buf;
}

bool hasRawControl(const std::string& s)
{
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 32)
            return true;
    return false;
}

// Recusa esperada: PathCheck/UnsupportedPath, nada criado/publicado, diagnóstico escapado. Com injeção,
// nenhum ponto interno (TempName, CreateTemp, ..., Publish) pode ser alcançado.
void expectControlRefused(const fs::path& p, ReplacePolicy pol, wchar_t c, std::size_t expectedPos,
                          const fs::path& watched, const std::vector<std::wstring>& treeBefore)
{
    // O caractere está de fato na representação nativa, na posição pretendida, e é o primeiro
    // caractere de controle do caminho.
    CHECK(p.native().size() > expectedPos && p.native()[expectedPos] == c);
    std::size_t firstControl = std::wstring::npos;
    for (std::size_t i = 0; i < p.native().size() && firstControl == std::wstring::npos; ++i)
        if (static_cast<unsigned>(p.native()[i]) < 32u)
            firstControl = i;
    CHECK(firstControl == expectedPos);
    int probes = 0;
    SafeWriteResult r;
    {
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
        ScopedHook hook([&](const fi::FaultContext&) {
            ++probes;
            return proceed();
        });
#endif
        r = writeFileSafely(p, "conteudo que nao pode ser publicado", opts(pol));
    }
    CHECK(probes == 0); // sem temporário, sem MoveFileExW
    CHECK(!r.ok);
    CHECK(r.stage == WriteStage::PathCheck);
    CHECK(r.error == SafeWriteError::UnsupportedPath);
    CHECK(r.win32Error == 0 && r.cleanupWin32Error == 0);
    CHECK(r.tempPath.empty() && r.tempRemoved && !r.tempPathForeign);
    CHECK(r.bytes == 0 && !r.destinationReplaced && r.destinationPreserved);
    CHECK(r.message.find('\0') == std::string::npos); // nenhum NUL bruto
    CHECK(!hasRawControl(r.message));                   // nenhum caractere de controle bruto
    CHECK(contains(r.message, "caractere de controle " + escaped(static_cast<unsigned>(c))));
    CHECK(contains(r.message, "na posição " + std::to_string(expectedPos)));
    CHECK(tree(watched) == treeBefore); // nenhum arquivo criado, removido ou temporário restante
}

void testControlChars()
{
    std::printf("[N] NUL e caracteres de controle em qualquer posição do caminho (E5.7.4b.8c.4d)\n");
    const wchar_t NUL = L'\0';

    {
        scenario("T01. NUL no nome final [Allow e Forbid]");
        const fs::path dir = g_root->sub("nul_nome");
        const fs::path p = dir / (L"a" + wch(NUL) + L"b.txt");
        CHECK(p.filename().native().size() == 7); // "a\0b.txt" com o NUL preservado
        for (ReplacePolicy pol : {ReplacePolicy::Allow, ReplacePolicy::Forbid})
            expectControlRefused(p, pol, NUL, dir.native().size() + 2, dir, {});
        CHECK(!pathExists(dir / L"a"));
    }

    {
        scenario("T02. NUL em componente de diretório com Allow: sentinela preservada");
        const fs::path dir = g_root->sub("nul_dir_allow");
        const fs::path sentinel = dir / L"vitima.txt";
        const std::string original = pattern(4321, 99);
        CHECK(writeRaw(sentinel, original));
        const auto idBefore = identityOf(sentinel);
        CHECK(idBefore.has_value());
        const fs::path p = dir / (L"vitima.txt" + wch(NUL) + L"x") / L"saida.txt";
        // A sentinela é só o PREFIXO do caminho até o NUL, não o destino solicitado.
        const std::size_t nulPos = p.native().find(NUL);
        CHECK(nulPos != std::wstring::npos && p.native().substr(0, nulPos) == sentinel.native());
        CHECK(p.native().size() > sentinel.native().size() && p != sentinel);
        const auto before = tree(dir);
        expectControlRefused(p, ReplacePolicy::Allow, NUL, nulPos, dir, before);
        const auto after = readFile(sentinel);
        CHECK(after.has_value() && *after == original); // bytes idênticos
        CHECK(identityOf(sentinel) == idBefore);          // mesma identidade e data
        CHECK(tempsOf(sentinel).empty() && tree(dir).size() == 1);
    }

    {
        scenario("T03. NUL em componente de diretório com Forbid: prefixo truncado não é criado");
        const fs::path dir = g_root->sub("nul_dir_forbid");
        const fs::path prefix = dir / L"novo";
        CHECK(!pathExists(prefix));
        const fs::path p = dir / (L"novo" + wch(NUL) + L"x") / L"saida.txt";
        const std::size_t nulPos = p.native().find(NUL);
        CHECK(nulPos != std::wstring::npos && p.native().substr(0, nulPos) == prefix.native());
        expectControlRefused(p, ReplacePolicy::Forbid, NUL, nulPos, dir, {});
        CHECK(!pathExists(prefix) && tempsOf(prefix).empty() && tree(dir).empty());
    }

    {
        scenario("T04. NUL em posições diferentes (relativo, intermediário, antes de separador, nome, fim)");
        const fs::path dir = g_root->sub("nul_posicoes");
        CHECK(::CreateDirectoryW((dir / L"sub").c_str(), nullptr) != 0);
        const auto before = tree(dir);
        struct Pos
        {
            const char* name;
            std::wstring path;
            bool relative;
        };
        const std::wstring d = dir.native();
        const Pos cases[] = {
            {"primeiro componente relativo", L"rel" + wch(NUL) + L"x\\sub\\saida.txt", true},
            {"componente intermediário", d + L"\\meio" + wch(NUL) + L"x\\sub\\saida.txt", false},
            {"imediatamente antes de separador", d + L"\\sub" + wch(NUL) + L"\\saida.txt", false},
            {"nome final (antes da extensão)", d + L"\\sub\\saida" + wch(NUL) + L".txt", false},
            {"último caractere da representação nativa", d + L"\\sub\\saida.txt" + wch(NUL), false},
        };
        wchar_t previousCwd[MAX_PATH + 1]{};
        const DWORD cwdLen = ::GetCurrentDirectoryW(MAX_PATH + 1, previousCwd);
        CHECK(cwdLen > 0 && cwdLen <= MAX_PATH);
        for (const Pos& c : cases) {
            std::printf("      %s\n", c.name);
            const std::size_t nulPos = c.path.find(NUL);
            CHECK(nulPos != std::wstring::npos);
            CHECK(std::count(c.path.begin(), c.path.end(), NUL) == 1);
            if (c.relative)
                CHECK(::SetCurrentDirectoryW(dir.c_str()) != 0); // relativo resolvido no diretório isolado
            for (ReplacePolicy pol : {ReplacePolicy::Allow, ReplacePolicy::Forbid})
                expectControlRefused(fs::path(c.path), pol, NUL, nulPos, dir, before);
            if (c.relative)
                CHECK(::SetCurrentDirectoryW(previousCwd) != 0);
        }
        CHECK(!pathExists(dir / L"rel") && !pathExists(dir / L"meio") && !pathExists(dir / L"sub" / L"saida.txt"));
        CHECK(tree(dir) == before);
    }

    {
        scenario("T05/T06. caracteres de controle 0-31 no diretório, no nome e no fim (diagnóstico \\xNN)");
        const fs::path dir = g_root->sub("controle");
        CHECK(::CreateDirectoryW((dir / L"sub").c_str(), nullptr) != 0);
        const auto before = tree(dir);
        const std::wstring d = dir.native();
        int refused = 0, total = 0;
        for (unsigned code = 0; code < 32; ++code) {
            const wchar_t c = static_cast<wchar_t>(code);
            const std::wstring paths[] = {
                d + L"\\pas" + wch(c) + L"ta\\saida.txt", // componente de diretório
                d + L"\\sub\\sa" + wch(c) + L"ida.txt",   // nome final
                d + L"\\sub\\saida.txt" + wch(c),         // fim da representação
            };
            for (const std::wstring& path : paths) {
                ++total;
                const int before_fail = cad::test::failures();
                expectControlRefused(fs::path(path), ReplacePolicy::Allow, c, path.find(c), dir, before);
                refused += cad::test::failures() == before_fail ? 1 : 0;
            }
        }
        std::printf("      %d de %d caminhos com caractere de controle recusados com \\xNN\n", refused, total);
        CHECK(refused == total);

        // Diagnóstico com Unicode válido preservado ao redor do caractere escapado.
        const fs::path u = dir / (L"ação_日本" + wch(NUL) + L"_\U0001F642.txt");
        const SafeWriteResult r = writeFileSafely(u, "x", opts(ReplacePolicy::Allow));
        CHECK(!r.ok && r.stage == WriteStage::PathCheck && r.error == SafeWriteError::UnsupportedPath);
        CHECK(contains(r.message, "ação_日本\\x00_\xF0\x9F\x99\x82.txt"));
        CHECK(contains(r.message, "\\x09") == false); // somente o caractere presente é mostrado
        CHECK(tree(dir) == before);
    }

    {
        scenario("T07. caminhos válidos continuam aceitos (relativo, absoluto, espaços, '/', Unicode)");
        const fs::path dir = g_root->sub("validos");
        CHECK(::CreateDirectoryW((dir / L"pasta com espaços").c_str(), nullptr) != 0);
        wchar_t previousCwd[MAX_PATH + 1]{};
        const DWORD cwdLen = ::GetCurrentDirectoryW(MAX_PATH + 1, previousCwd);
        CHECK(cwdLen > 0 && cwdLen <= MAX_PATH);
        CHECK(::SetCurrentDirectoryW(dir.c_str()) != 0);
        const SafeWriteResult rel = writeFileSafely(fs::path(L"relativo.txt"), "rel", opts(ReplacePolicy::Forbid));
        const SafeWriteResult relSub =
            writeFileSafely(fs::path(L"pasta com espaços\\arquivo com espaços.txt"), "esp", opts(ReplacePolicy::Forbid));
        const SafeWriteResult dotDot =
            writeFileSafely(fs::path(L"pasta com espaços\\..\\ponto_ponto.txt"), "pp", opts(ReplacePolicy::Forbid));
        CHECK(::SetCurrentDirectoryW(previousCwd) != 0);
        expectWritten(rel, dir / L"relativo.txt", "rel");
        expectWritten(relSub, dir / L"pasta com espaços" / L"arquivo com espaços.txt", "esp");
        expectWritten(dotDot, dir / L"ponto_ponto.txt", "pp");
        const fs::path slash = fs::path(dir.native() + L"/barra_normal.txt");
        expectWritten(writeFileSafely(slash, "barra", opts(ReplacePolicy::Forbid)), dir / L"barra_normal.txt", "barra");
        const fs::path uni = dir / L"ünïcødé_日本語_\U0001F642 final.txt";
        expectWritten(writeFileSafely(uni, "uni", opts(ReplacePolicy::Forbid)), uni, "uni");
        // Caracteres >= 32 próximos da faixa recusada continuam aceitos (espaço interno, '!', DEL).
        const fs::path edge = dir / (L"a b!" + wch(L'\x7F') + L".txt");
        expectWritten(writeFileSafely(edge, "limite", opts(ReplacePolicy::Forbid)), edge, "limite");
    }
}

// =============================================================================================
// [R] DEM_DYNA.txt de referência gravado pelo SafeFileWriter (sem composeDemDyna).
void testReference()
{
    std::printf("[R] DEM_DYNA.txt de referência\n");
    scenario("DEM_DYNA.txt (somente leitura) gravado no diretório isolado");
    const auto ref = readFile(fs::u8path(CAD_MATLAB_REFERENCE_DIR "/DEM_DYNA.txt"));
    CHECK(ref.has_value());
    if (!ref)
        return;
    CHECK(ref->size() == 58181);
    const fs::path dir = g_root->sub("demdyna");
    const fs::path dest = dir / L"DEM_DYNA.txt";
    const SafeWriteResult r = writeFileSafely(dest, *ref, opts(ReplacePolicy::Forbid));
    expectWritten(r, dest, *ref);
    CHECK(r.bytes == 58181);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    CHECK(::GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 58181 &&
          fa.nFileSizeHigh == 0);
    std::printf("      %llu bytes gravados, idênticos à referência\n", static_cast<unsigned long long>(r.bytes));
}

// =============================================================================================
// [M] Contrato de publicação (E5.7.4b.8c.4c).
constexpr const char* kShortCreated = "arquivo criado";
constexpr const char* kShortReplaced = "substituído";
constexpr const char* kShortInternal = "erro interno";

// Chamada protegida: registra se alguma exceção escapou de writeFileSafely (violação do contrato).
struct Guarded
{
    SafeWriteResult r;
    bool escaped = false;
};

Guarded callGuarded(const fs::path& dest, const std::string& data, const SafeWriteOptions& o)
{
    Guarded g;
    try {
        g.r = writeFileSafely(dest, data, o);
    } catch (...) {
        g.escaped = true;
    }
    return g;
}

// Coerência geral de qualquer resultado + estado real do disco. 'original' vazio = destino ausente.
// Devolve true se o arquivo foi efetivamente publicado (conteúdo = data).
bool checkCoherent(const SafeWriteResult& r, const fs::path& dest, const std::string& data,
                   const std::optional<std::string>& original)
{
    const auto now = readFile(dest);
    const bool published = now.has_value() && *now == data;
    const bool untouched = original.has_value() ? (now.has_value() && *now == *original) : !now.has_value();
    CHECK(published || untouched);
    CHECK(r.ok == (r.stage == WriteStage::Done)); // Done somente com ok
    CHECK(r.ok == published);                     // publicação efetiva ⇔ sucesso
    CHECK(!r.message.empty());
    CHECK(!r.tempPathForeign);
    const auto temps = tempsOf(dest);
    CHECK(r.tempRemoved ? temps.empty() : !temps.empty());
    for (const auto& t : temps) // remoção manual de eventual sobra (somente no diretório de teste)
        ::DeleteFileW((dest.parent_path() / t).c_str());
    if (r.ok) {
        CHECK(r.error == SafeWriteError::None && r.win32Error == 0);
        CHECK(r.bytes == data.size() && r.tempRemoved);
        CHECK(r.destinationReplaced == original.has_value());
        CHECK(r.destinationPreserved == !original.has_value()); // Forbid confirma; Allow não afirma
    } else {
        CHECK(!r.destinationReplaced);
    }
    return published;
}

// Casos A–C com todos os campos do resultado (executado também na biblioteca de produção).
void testPublicationCases()
{
    std::printf("[M] contrato de publicação: casos A-C\n");
    {
        scenario("M-A. destino inexistente: publicação bem-sucedida");
        const fs::path dir = g_root->sub("mA");
        const fs::path dest = dir / L"saida.txt";
        const std::string data = pattern(70000, 31);
        const Guarded g = callGuarded(dest, data, opts(ReplacePolicy::Forbid));
        CHECK(!g.escaped);
        const SafeWriteResult& r = g.r;
        CHECK(checkCoherent(r, dest, data, std::nullopt));
        CHECK(r.ok && r.stage == WriteStage::Done && r.error == SafeWriteError::None && r.win32Error == 0);
        CHECK(r.cleanupWin32Error == 0 && !r.destinationExisted && !r.destinationReplaced && r.destinationPreserved);
        CHECK(r.tempRemoved && !r.tempPathForeign && !r.tempPath.empty() && !pathExists(r.tempPath));
        CHECK(r.bytes == data.size() && contains(r.message, "arquivo criado: "));
        CHECK(entriesIn(dir) == 1);
    }
    {
        scenario("M-B. destino existente com Forbid: recusa, original preservado");
        const fs::path dir = g_root->sub("mB");
        const fs::path dest = dir / L"saida.txt";
        const std::string original = pattern(1234, 32);
        CHECK(writeRaw(dest, original));
        const Guarded g = callGuarded(dest, pattern(5000, 33), opts(ReplacePolicy::Forbid));
        CHECK(!g.escaped);
        const SafeWriteResult& r = g.r;
        CHECK(!r.ok && r.stage == WriteStage::PathCheck && r.error == SafeWriteError::DestinationExists);
        CHECK(r.win32Error == 0 && r.cleanupWin32Error == 0);
        CHECK(r.destinationExisted && !r.destinationReplaced && r.destinationPreserved);
        CHECK(r.tempRemoved && !r.tempPathForeign && r.tempPath.empty() && r.bytes == 0);
        const auto after = readFile(dest);
        CHECK(after.has_value() && *after == original);
        CHECK(entriesIn(dir) == 1);
    }
    {
        scenario("M-C. destino existente com Allow: substituição autorizada");
        const fs::path dir = g_root->sub("mC");
        const fs::path dest = dir / L"saida.txt";
        CHECK(writeRaw(dest, pattern(1234, 34)));
        const std::string data = pattern(9000, 35);
        const Guarded g = callGuarded(dest, data, opts(ReplacePolicy::Allow));
        CHECK(!g.escaped);
        const SafeWriteResult& r = g.r;
        CHECK(checkCoherent(r, dest, data, pattern(1234, 34)));
        CHECK(r.ok && r.stage == WriteStage::Done && r.error == SafeWriteError::None);
        CHECK(r.destinationExisted && r.destinationReplaced && !r.destinationPreserved);
        CHECK(r.tempRemoved && !r.tempPathForeign && contains(r.message, "arquivo substituído: "));
        CHECK(entriesIn(dir) == 1);
    }
}

// Casos D/G: falha de alocação injetada em CADA alocação do gravador (modo único e persistente).
struct SweepTally
{
    int runs = 0, escaped = 0, ok = 0, okFallback = 0, internal = 0, internalShort = 0, injected = 0;
};

void testAllocationSweep()
{
    std::printf("[M] falhas de alocação em cada alocação do gravador\n");
    const std::string data = pattern(5000, 41);
    const std::string original = pattern(3000, 42);
    for (const bool existing : {false, true}) {
        // Contagem de referência (sem falhas).
        long long total = 0;
        {
            const fs::path dir = g_root->sub("mcount");
            const fs::path dest = dir / L"saida.txt";
            if (existing)
                CHECK(writeRaw(dest, original));
            oom::arm(0, false);
            const Guarded g = callGuarded(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid));
            total = oom::disarm();
            CHECK(!g.escaped && g.r.ok);
        }
        std::printf("      alocações contadas numa gravação completa [%s]: %lld\n",
                    existing ? "existente/Allow" : "ausente/Forbid", total);
        CHECK(total > 0);
        for (const bool persistent : {false, true}) {
            const std::string title = std::string("M-D/G. falha ") + (persistent ? "persistente" : "isolada") +
                                      " na n-ésima alocação, n = 1.." + std::to_string(total + 1) +
                                      (existing ? " [existente/Allow]" : " [ausente/Forbid]");
            scenario(title.c_str());
            SweepTally t;
            for (long long k = 1; k <= total + 1; ++k) {
                const fs::path dir = g_root->sub("msweep");
                const fs::path dest = dir / L"saida.txt";
                if (existing)
                    CHECK(writeRaw(dest, original));
                oom::arm(k, persistent);
                const Guarded g = callGuarded(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid));
                oom::disarm();
                t.injected += oom::tInjected > 0 ? 1 : 0;
                ++t.runs;
                CHECK(!g.escaped);
                if (g.escaped) {
                    ++t.escaped;
                    continue;
                }
                const SafeWriteResult& r = g.r;
                checkCoherent(r, dest, data, existing ? std::optional<std::string>(original) : std::nullopt);
                if (r.ok) {
                    ++t.ok;
                    if (r.message == (existing ? kShortReplaced : kShortCreated))
                        ++t.okFallback;
                    else
                        CHECK(contains(r.message, existing ? "arquivo substituído: " : "arquivo criado: "));
                } else {
                    ++t.internal;
                    CHECK(r.error == SafeWriteError::Internal && r.stage != WriteStage::Done);
                    CHECK(r.destinationPreserved); // falha antes da publicação: destino não tocado
                    if (r.message == kShortInternal)
                        ++t.internalShort;
                    else
                        CHECK(contains(r.message, "erro interno"));
                }
            }
            std::printf("      execuções %d: sucesso %d (mensagem curta %d), erro interno antes da publicação %d "
                        "(mensagem curta %d), com falha injetada %d, exceções escapadas %d\n",
                        t.runs, t.ok, t.okFallback, t.internal, t.internalShort, t.injected, t.escaped);
            CHECK(t.escaped == 0);
            CHECK(t.internal > 0); // falhas antes da publicação exercitadas
            CHECK(t.ok > 0);
            CHECK(t.okFallback > 0); // preparação da mensagem sem memória → publicação com texto curto
        }
    }
}

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
// Caso F: falha de alocação armada exatamente no ponto da publicação (FaultPoint::Publish, imediatamente
// antes da MoveFileExW). mode 0 = só contar; 1 = falha na 1ª alocação seguinte; 2 = todas falham.
void runPostPublish(bool existing, int mode)
{
    static const char* const kModes[] = {"contagem", "falha isolada", "falha persistente"};
    const std::string title = std::string("M-F. alocação após a publicação (") + kModes[mode] + ")" +
                              (existing ? " [existente/Allow]" : " [ausente/Forbid]");
    scenario(title.c_str());
    const fs::path dir = g_root->sub("mF");
    const fs::path dest = dir / L"saida.txt";
    const std::string original = pattern(2500, 51);
    if (existing)
        CHECK(writeRaw(dest, original));
    const std::string data = pattern(40000, 52);
    bool armed = false;
    Guarded g;
    long long after = -1;
    long long injected = -1;
    {
        ScopedHook hook([&](const fi::FaultContext& c) {
            if (c.point == fi::FaultPoint::Publish) {
                armed = true;
                oom::arm(mode == 0 ? 0 : 1, mode == 2); // último comando do gancho
            }
            return proceed();
        });
        g = callGuarded(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid));
        after = oom::disarm();
        injected = oom::tInjected;
    }
    std::printf("      alocações depois do ponto de publicação: %lld (falhas injetadas: %lld); exceção escapou: %s\n",
                after, injected, g.escaped ? "SIM" : "não");
    printResult("", g.r);
    CHECK(armed);
    CHECK(!g.escaped);
    CHECK(after == 0); // nenhuma alocação depois da MoveFileExW (auditoria demonstrada)
    CHECK(injected == 0);
    if (g.escaped)
        return;
    const SafeWriteResult& r = g.r;
    CHECK(checkCoherent(r, dest, data, existing ? std::optional<std::string>(original) : std::nullopt));
    CHECK(r.ok && r.stage == WriteStage::Done && r.error == SafeWriteError::None);
    CHECK(contains(r.message, existing ? "arquivo substituído: " : "arquivo criado: "));
    const auto now = readFile(dest); // conteúdo final conferido independentemente do resultado
    CHECK(now.has_value() && *now == data);
}

// Preparação da mensagem de sucesso sem memória (armada em BeforePublish): publica com texto curto.
void runPreparedMessageFallback(bool existing, bool persistent)
{
    const std::string title = std::string("M-F2. mensagem de sucesso sem memória antes da publicação (") +
                              (persistent ? "persistente" : "isolada") + ")" +
                              (existing ? " [existente/Allow]" : " [ausente/Forbid]");
    scenario(title.c_str());
    const fs::path dir = g_root->sub("mF2");
    const fs::path dest = dir / L"saida.txt";
    const std::string original = pattern(2500, 53);
    if (existing)
        CHECK(writeRaw(dest, original));
    const std::string data = pattern(40000, 54);
    Guarded g;
    long long injected = -1;
    {
        ScopedHook hook([&](const fi::FaultContext& c) {
            if (c.point == fi::FaultPoint::BeforePublish)
                oom::arm(1, persistent);
            return proceed();
        });
        g = callGuarded(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid));
        oom::disarm();
        injected = oom::tInjected;
    }
    printResult("", g.r);
    CHECK(!g.escaped);
    CHECK(injected >= 1);
    if (g.escaped)
        return;
    const SafeWriteResult& r = g.r;
    CHECK(checkCoherent(r, dest, data, existing ? std::optional<std::string>(original) : std::nullopt));
    CHECK(r.ok && r.stage == WriteStage::Done && r.error == SafeWriteError::None);
    CHECK(r.message == (existing ? kShortReplaced : kShortCreated));
}

// Caso E: MoveFileExW falha e a montagem do diagnóstico fica sem memória. A preservação do destino
// precisa ser a CONFIRMADA (não o valor inicial presumido); o temporário é removido.
void runPublishFailureOom(bool thirdParty, bool persistent)
{
    const std::string title = std::string("M-E. MoveFileExW falha + sem memória no diagnóstico (") +
                              (persistent ? "persistente" : "isolada") + ")" +
                              (thirdParty ? ", destino alterado por terceiro" : ", destino intacto");
    scenario(title.c_str());
    const fs::path dir = g_root->sub("mE");
    const fs::path dest = dir / L"saida.txt";
    const std::string original = pattern(2500, 55);
    const std::string foreign = "alterado por terceiro, com outro tamanho";
    CHECK(writeRaw(dest, original));
    Guarded g;
    long long injected = -1;
    {
        ScopedHook hook([&](const fi::FaultContext& c) {
            if (c.point == fi::FaultPoint::Publish) {
                if (thirdParty)
                    writeRaw(dest, foreign);
                const fi::FaultDecision d = failWith(ERROR_ACCESS_DENIED);
                oom::arm(1, persistent); // último comando do gancho
                return d;
            }
            return proceed();
        });
        g = callGuarded(dest, pattern(40000, 56), opts(ReplacePolicy::Allow));
        oom::disarm();
        injected = oom::tInjected;
    }
    printResult("", g.r);
    CHECK(!g.escaped);
    CHECK(injected >= 1);
    if (g.escaped)
        return;
    const SafeWriteResult& r = g.r;
    CHECK(!r.ok && r.stage == WriteStage::Publish);
    CHECK(r.error == SafeWriteError::Internal || r.error == SafeWriteError::PublishFailed);
    CHECK(r.destinationPreserved == !thirdParty); // confirmado pela consulta, nunca presumido
    CHECK(!r.destinationReplaced && r.tempRemoved && tempsOf(dest).empty());
    CHECK(!r.message.empty());
    const auto now = readFile(dest);
    CHECK(now.has_value() && *now == (thirdParty ? foreign : original));
}

// Caso I: cancelamento pedido no ponto da publicação, com o resto da operação sem memória: a
// publicação conclui e o resultado não é "cancelado".
void runLateCancelOom(bool existing)
{
    scenario(existing ? "M-I. cancelamento após iniciada a publicação, sem memória [existente/Allow]"
                      : "M-I. cancelamento após iniciada a publicação, sem memória [ausente/Forbid]");
    const fs::path dir = g_root->sub("mI");
    const fs::path dest = dir / L"saida.txt";
    const std::string original = pattern(2500, 57);
    if (existing)
        CHECK(writeRaw(dest, original));
    const std::string data = pattern(40000, 58);
    std::atomic<bool> cancel{false};
    Guarded g;
    {
        ScopedHook hook([&](const fi::FaultContext& c) {
            if (c.point == fi::FaultPoint::Publish) {
                cancel.store(true);
                oom::arm(1, true);
            }
            return proceed();
        });
        g = callGuarded(dest, data, opts(existing ? ReplacePolicy::Allow : ReplacePolicy::Forbid, &cancel));
        oom::disarm();
    }
    CHECK(cancel.load());
    CHECK(!g.escaped);
    if (g.escaped)
        return;
    const SafeWriteResult& r = g.r;
    CHECK(checkCoherent(r, dest, data, existing ? std::optional<std::string>(original) : std::nullopt));
    CHECK(r.ok && r.error != SafeWriteError::Cancelled);
}

void testPostPublication()
{
    std::printf("[M] ponto de publicação (injeção de falhas)\n");
    for (const bool existing : {false, true})
        for (int mode = 0; mode < 3; ++mode)
            runPostPublish(existing, mode);
    for (const bool existing : {false, true})
        for (const bool persistent : {false, true})
            runPreparedMessageFallback(existing, persistent);
    for (const bool thirdParty : {false, true})
        for (const bool persistent : {false, true})
            runPublishFailureOom(thirdParty, persistent);
    for (const bool existing : {false, true})
        runLateCancelOom(existing);
}
#endif // CAD_SAFE_WRITER_FAULT_INJECTION

} // namespace

int main()
{
    std::printf("E5.7.4b.8c.3 - SafeFileWriter: falhas, cancelamento, concorrência e recuperação (%s)\n",
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
                "com injeção de falhas"
#else
                "biblioteca de produção, sem injeção"
#endif
    );
    fs::path rootPath;
    {
    TestRoot root;
    CHECK(root.valid());
    if (!root.valid())
        return cad::test::finish("e574b8c");
    g_root = &root;
    rootPath = root.path();
    std::printf("diretório isolado: %s\n", utf8(root.path().native()).c_str());

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    testFaults();
    testCancellation();
#endif
    testCancelBeforeStart();
    testConcurrency();
    testPaths();
    testLinks();
    testControlChars();
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    testTempIdentity();
    testContract();
#endif
    testReference();
    testPublicationCases();
    testAllocationSweep();
#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    testPostPublication();
#endif

#if defined(CAD_SAFE_WRITER_FAULT_INJECTION)
    std::printf("\nMatriz de falhas (ok=false/etapa/motivo | destino | temporário | diagnóstico):\n");
    for (const MatrixRow& m : g_matrix)
        std::printf("  %-3s %-16s %-11s %-17s win32=%-5lu  %s | %s | %s | %s\n", m.id.c_str(), m.variant.c_str(),
                    m.stage.c_str(), m.error.c_str(), m.win32, m.okFalse ? "OK" : "FALHA", m.destOk ? "OK" : "FALHA",
                    m.tempOk ? "OK" : "FALHA", m.diagOk ? "OK" : "FALHA");
#endif
    std::printf("\ncenários: %d; verificações: %d; não executados: %zu\n", g_scenarios, g_checks, g_notRun.size());
    for (const std::string& s : g_notRun)
        std::printf("  NAO EXECUTADO: %s\n", s.c_str());

    g_root = nullptr;
    } // ~TestRoot: remove o diretório isolado (sem seguir junções/links)
    CHECK(!pathExists(rootPath));
    std::printf("diretório isolado removido: %s\n", pathExists(rootPath) ? "NAO" : "sim");
    return cad::test::finish("e574b8c");
}
