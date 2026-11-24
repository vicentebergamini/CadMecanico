// E5.7.4b.8c.4 sem interface — exportação DEM-DYNA (discretization/DemDynaExporter.h): composeDemDyna
// + writeFileSafely, contra a cad_discretization de produção. Todas as escritas ocorrem num diretório
// exclusivo criado em %TEMP% para esta execução e removido no final. As referências MATLAB/APDL são
// somente lidas.
//
//  A. modelo válido, destino inexistente            H. gravação falha: destino bloqueado
//  B. destino existente, Forbid (padrão)             I. cancelamento antes da publicação
//  C. destino existente, Allow explícito             J. arquivo = composeDemDyna(input).text
//  D. modelo inválido, destino inexistente          K. DEM_DYNA.txt: 58.181 bytes idênticos
//  E. modelo inválido, destino existente            L. nenhum temporário após sucesso
//  F. validação constitutiva (tp = 3)                M. diagnósticos separados (composição × gravação)
//  G. colapso de coordenadas com %g                  N. composição recusada → nenhuma escrita
//  P. proteções: entrada inalterada, caminhos inválidos sem sucesso falso, política repassada;
//     T08 (E5.7.4b.8c.4d): NUL no nome e em componente de diretório recusado pelo gravador
//     (PathCheck/UnsupportedPath), sentinela preservada, nenhum arquivo no prefixo truncado.
//  Q. falhas de alocação injetadas (E5.7.4b.8c.4b): nenhuma exceção sai de exportDemDyna e o
//     resultado continua coerente (P1: mensagem após a publicação; P2: exceção antes da gravação;
//     P3: exceção dentro de writeFileSafely → estado "não confirmado"). E5.7.4b.8c.4c: com o
//     gravador corrigido, writeFileSafely não lança e nunca devolve stage = Done com ok = false
//     (P3 e esse ramo ficam defensivos); Q-H repete a injeção com destino existente/Allow.
#include "TestCheck.h"

#include "dem/DemProperties.h"
#include "dem/LatticeConstants.h"
#include "dem/LawValidation.h"
#include "dem/MatlabLawTable.h"
#include "discretization/ApdlExporter.h" // ApdlLinkSequence (somente leitura)
#include "discretization/DemDynaApdl.h"
#include "discretization/DemDynaExporter.h"
#include "discretization/LatticeTopology.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Injeção de falhas de alocação (somente neste executável de teste; cenário Q). O operator new
// global é substituído; desarmado (padrão), apenas repassa a malloc. Armado, conta as alocações da
// thread atual e faz falhar a n-ésima (modo único) ou todas a partir dela (modo persistente, "pouca
// memória"). Em Debug (_ITERATOR_DEBUG_LEVEL != 0) as alocações do tamanho de std::_Container_proxy
// não são contadas: o MSVC as faz dentro de construtores noexcept (falhar nelas encerraria o
// processo, não testaria o exportador).
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
namespace dem = cad::dem;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_scenarios = 0;
#define CHECK(c) (++g_checks, CAD_CHECK(c))

void scenario(const char* name)
{
    ++g_scenarios;
    std::printf("  %s\n", name);
}

// ---------------------------------------------------------------------------------------------
// Arquivos (somente no diretório isolado).
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

// Instantâneo do diretório: nome → (tamanho, data de modificação, índice do arquivo).
struct Entry
{
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    std::uint64_t index = 0;
    bool operator==(const Entry& o) const { return size == o.size && mtime == o.mtime && index == o.index; }
};
std::map<std::wstring, Entry> snapshot(const fs::path& dir)
{
    std::map<std::wstring, Entry> out;
    WIN32_FIND_DATAW fd{};
    HANDLE f = ::FindFirstFileW((dir / L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..")
            continue;
        Entry e;
        e.size = (std::uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = (std::uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        HANDLE h = ::CreateFileW((dir / n).c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION bi{};
            if (::GetFileInformationByHandle(h, &bi))
                e.index = (std::uint64_t(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
            ::CloseHandle(h);
        }
        out[n] = e;
    } while (::FindNextFileW(f, &fd));
    ::FindClose(f);
    return out;
}

std::size_t tmpCount(const fs::path& dir)
{
    std::size_t n = 0;
    for (const auto& [name, e] : snapshot(dir))
        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".tmp") == 0)
            ++n;
    return n;
}

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
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(p.c_str()) : ::DeleteFileW(p.c_str());
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                removeTree(p);
            else
                ::DeleteFileW(p.c_str());
        } while (::FindNextFileW(f, &fd));
        ::FindClose(f);
    }
    ::RemoveDirectoryW(dir.c_str());
}

class TestRoot
{
public:
    TestRoot()
    {
        wchar_t tmp[MAX_PATH + 1]{};
        const DWORD n = ::GetTempPathW(MAX_PATH + 1, tmp);
        const fs::path base = n > 0 ? fs::path(tmp) : fs::path(L"C:\\Temp");
        for (int i = 0; i < 64 && root_.empty(); ++i) {
            wchar_t name[96];
            std::swprintf(name, 96, L"cad_e574b8c4_%lx_%llx_%d", ::GetCurrentProcessId(),
                          static_cast<unsigned long long>(::GetTickCount64()), i);
            if (::CreateDirectoryW((base / name).c_str(), nullptr))
                root_ = base / name;
        }
    }
    ~TestRoot()
    {
        if (!root_.empty() && root_.filename().native().rfind(L"cad_e574b8c4_", 0) == 0)
            removeTree(root_);
    }
    bool valid() const { return !root_.empty(); }
    const fs::path& path() const { return root_; }
    fs::path sub(const char* tag)
    {
        const fs::path p = root_ / (std::to_wstring(++counter_) + L"_" + std::wstring(tag, tag + std::strlen(tag)));
        ::CreateDirectoryW(p.c_str(), nullptr);
        return p;
    }

private:
    fs::path root_;
    int counter_ = 0;
};
TestRoot* g_root = nullptr;

// ---------------------------------------------------------------------------------------------
// Modelos (mesma construção de test_e574b8).
Discretization buildBox(int nx, int ny, int nz, double originMm, const std::function<bool(int, int, int)>& active)
{
    VoxelGrid g = VoxelGrid::make(gp_XYZ(originMm, 0.0, 0.0), 1.0);
    g.nx = nx;
    g.ny = ny;
    g.nz = nz;
    Parameters p;
    p.lco = 1.0;
    p.criterion = InclusionCriterion::CenterInside;
    std::vector<std::uint8_t> on(std::size_t(nx) * ny * nz, 0);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                on[std::size_t((std::int64_t(k) * ny + j) * nx + i)] = active(i, j, k) ? 1 : 0;
    Discretization d;
    CHECK(d.build(g, p, on));
    return d;
}

Discretization caixa(double originMm = 0.0)
{
    return buildBox(4, 4, 2, originMm, [](int, int, int) { return true; });
}
Discretization ele() // canto reentrante: barras tp = 3 (lei inválida em barra ativa)
{
    return buildBox(4, 4, 2, 0.0, [](int i, int j, int) { return !(i >= 2 && j >= 2); });
}

std::string between(const std::string& text, const std::string& from, const std::string& to)
{
    const std::size_t a = text.find(from);
    const std::size_t b = a == std::string::npos ? std::string::npos : text.find(to, a);
    return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b - a);
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t s = 0;
    while (s < text.size()) {
        const std::size_t e = text.find('\n', s);
        out.push_back(text.substr(s, e == std::string::npos ? std::string::npos : e - s));
        if (e == std::string::npos)
            break;
        s = e + 1;
    }
    return out;
}

// Entrada do DEM_DYNA.txt de referência (Gf por barra reconstruído como em test_e574b8 [A]).
std::optional<DemDynaInput> referenceInput(const Discretization& d, const std::string& ref)
{
    const std::vector<std::string> refLines = splitLines(ref);
    if (refLines.size() < 8)
        return std::nullopt;
    const LatticeTopology topo = classifyLattice(d);
    const dem::LatticeConstants c = dem::computeLattice(dem::matlabDefaults(), 1e-3);
    const dem::WeibullClasses w = dem::weibullBins(0.8, 20);
    const std::string refLaws = between(ref, "!************************INICIO DA LEI 1\n", "! ***********************DEFINE");
    const std::string refEmodif = between(ref, "! ***********************DEFINE", "ESEL,S,TYPE,,1\n");
    std::vector<double> fpOfLaw(1, 0.0);
    std::size_t q = 0;
    while ((q = refLaws.find("TBDAT,4,", q)) != std::string::npos)
        fpOfLaw.push_back(std::strtod(refLaws.c_str() + (q += 8), nullptr));
    std::vector<int> lawOfBond;
    q = 0;
    while ((q = refEmodif.find(",MAT,", q)) != std::string::npos)
        lawOfBond.push_back(int(std::strtod(refEmodif.c_str() + (q += 5), nullptr)));
    if (fpOfLaw.size() != 52 || lawOfBond.size() != 490 || topo.links() != 490)
        return std::nullopt;
    std::vector<double> gf(490);
    for (std::size_t b = 0; b < 490; ++b) {
        const int tp = topo.linkTp[b];
        const double ea = tp == 1 ? c.enr / 4.0 : tp == 2 ? c.enr / 2.0 : tp == 4 ? c.enr : c.edg;
        const double approx = fpOfLaw[std::size_t(lawOfBond[b])] / (ea * c.epr);
        std::size_t best = 0;
        for (std::size_t i = 1; i < w.xm.size(); ++i)
            if (std::fabs(w.xm[i] - approx) < std::fabs(w.xm[best] - approx))
                best = i;
        gf[b] = w.xm[best];
    }
    DemDynaInput in;
    in.discretization = &d;
    in.properties = dem::matlabDefaults();
    in.gf = gf;
    in.header.seed = 10.0;
    in.header.timestamp = refLines[3].substr(std::string("!Code generate on ").size());
    in.header.workingDirectory = refLines[4].substr(6, refLines[4].size() - 7);
    in.gfField = DemDynaGfFieldNote{{0.0, 0.0, 45.0}};
    return in;
}

DemDynaInput simpleInput(const Discretization& d)
{
    DemDynaInput in;
    in.discretization = &d;
    in.properties = dem::matlabDefaults();
    in.gf.assign(std::size_t(ApdlLinkSequence::count(d).total()), 1.0);
    return in;
}

SafeWriteOptions opts(ReplacePolicy p, const std::atomic<bool>* cancel = nullptr)
{
    SafeWriteOptions o;
    o.replace = p;
    o.cancel = cancel;
    return o;
}

void show(const DemDynaExportResult& r)
{
    std::printf("    ok=%d etapa=%s composicao.ok=%d erros=%zu gravacaoIniciada=%d gravacao=%s/%s win32=%lu "
                "compostos=%llu gravados=%llu publicados=%llu tempRemovido=%d preservado=%d\n",
                r.ok ? 1 : 0, demDynaExportStageName(r.stage), r.composition.ok ? 1 : 0, r.composition.errors.size(),
                r.writeStarted ? 1 : 0, writeStageName(r.write.stage), safeWriteErrorName(r.write.error),
                static_cast<unsigned long>(r.write.win32Error), (unsigned long long)r.bytesComposed,
                (unsigned long long)r.bytesWritten, (unsigned long long)r.bytesPublished, r.write.tempRemoved ? 1 : 0,
                r.write.destinationPreserved ? 1 : 0);
}

bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

// Composição recusada: nenhuma gravação, resultado de gravação explicitamente "não iniciada".
void expectNotWritten(const DemDynaExportResult& r)
{
    CHECK(!r.ok);
    CHECK(r.stage == DemDynaExportStage::Composition);
    CHECK(!r.composition.ok && r.composition.text.empty() && !r.composition.errors.empty());
    CHECK(!r.writeStarted);
    CHECK(!r.write.ok && r.write.error == SafeWriteError::None && r.write.bytes == 0);
    CHECK(r.write.tempPath.empty() && r.write.tempRemoved && r.write.destinationPreserved);
    CHECK(contains(r.write.message, "gravação não iniciada"));
    CHECK(r.bytesComposed == 0 && r.bytesWritten == 0 && r.bytesPublished == 0);
    CHECK(contains(r.message, "recusada na composição"));
}

// ---------------------------------------------------------------------------------------------
void testValid(const std::string& ref)
{
    std::printf("[A-C, J-L] modelo válido\n");
    const Discretization d = caixa();
    const std::optional<DemDynaInput> refIn = referenceInput(d, ref);
    CHECK(refIn.has_value());
    if (!refIn)
        return;
    const DemDynaInput& in = *refIn;
    const DemDynaComposition direct = composeDemDyna(in);
    CHECK(direct.ok && direct.text.size() == 58181 && direct.text == ref);

    const fs::path dir = g_root->sub("valido");
    const fs::path dest = dir / L"DEM_DYNA.txt";

    scenario("A. modelo válido, destino inexistente (Forbid padrão)");
    const DemDynaExportResult a = exportDemDyna(in, dest); // opções padrão
    show(a);
    CHECK(a.ok && a.stage == DemDynaExportStage::Done && a.writeStarted && a.write.ok);
    CHECK(a.composition.ok && a.composition.errors.empty() && a.composition.lawsValidated);
    CHECK(a.destination == dest);
    CHECK(!a.write.destinationExisted && !a.write.destinationReplaced && a.write.destinationPreserved);
    CHECK(a.bytesComposed == 58181 && a.bytesWritten == 58181 && a.bytesPublished == 58181);

    scenario("J. arquivo exportado = composeDemDyna(input).text");
    const auto onDisk = readFile(dest);
    CHECK(onDisk.has_value() && *onDisk == direct.text && *onDisk == a.composition.text);

    scenario("K. equivalência byte a byte com DEM_DYNA.txt (58.181 bytes)");
    CHECK(onDisk.has_value() && *onDisk == ref && onDisk->size() == 58181);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    CHECK(::GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 &&
          fa.nFileSizeLow == 58181);
    std::printf("    arquivo gravado: %zu bytes, %s ao DEM_DYNA.txt\n", onDisk ? onDisk->size() : 0,
                onDisk && *onDisk == ref ? "idêntico byte a byte" : "DIFERENTE");

    scenario("L. nenhum temporário após sucesso");
    CHECK(tmpCount(dir) == 0 && snapshot(dir).size() == 1 && a.write.tempRemoved);

    scenario("B. destino existente com Forbid (padrão e explícito)");
    const std::string previous = "conteúdo anterior do usuário\r\n";
    CHECK(writeRaw(dest, previous));
    const auto before = snapshot(dir);
    for (const DemDynaExportResult& b : {exportDemDyna(in, dest), exportDemDyna(in, dest, opts(ReplacePolicy::Forbid))}) {
        show(b);
        CHECK(!b.ok && b.stage == DemDynaExportStage::Write && b.writeStarted);
        CHECK(b.composition.ok && b.composition.text == direct.text); // composição aceita é preservada
        CHECK(b.write.stage == WriteStage::PathCheck && b.write.error == SafeWriteError::DestinationExists);
        CHECK(b.write.destinationExisted && b.write.destinationPreserved && b.write.tempRemoved);
        CHECK(b.bytesComposed == 58181 && b.bytesWritten == 0 && b.bytesPublished == 0);
        CHECK(contains(b.message, "gravação falhou"));
    }
    CHECK(snapshot(dir) == before);
    CHECK(readFile(dest) == std::optional<std::string>(previous));

    scenario("C. destino existente com Allow explícito");
    const DemDynaExportResult c = exportDemDyna(in, dest, opts(ReplacePolicy::Allow));
    show(c);
    CHECK(c.ok && c.write.destinationExisted && c.write.destinationReplaced && !c.write.destinationPreserved);
    CHECK(readFile(dest) == std::optional<std::string>(ref));
    CHECK(tmpCount(dir) == 0);
}

void testInvalid()
{
    std::printf("[D-G, N] composição recusada\n");
    const Discretization bad = ele();
    const DemDynaInput lawInput = simpleInput(bad);

    {
        scenario("D/N. modelo inválido, destino inexistente: nenhum arquivo, nenhum temporário");
        const fs::path dir = g_root->sub("invalido_novo");
        const fs::path dest = dir / L"DEM_DYNA.txt";
        const DemDynaExportResult r = exportDemDyna(lawInput, dest, opts(ReplacePolicy::Allow));
        show(r);
        expectNotWritten(r);
        CHECK(!pathExists(dest) && snapshot(dir).empty());
    }
    {
        scenario("E/N. modelo inválido, destino existente: conteúdo e identidade preservados");
        const fs::path dir = g_root->sub("invalido_existente");
        const fs::path dest = dir / L"DEM_DYNA.txt";
        CHECK(writeRaw(dest, "arquivo anterior"));
        const auto before = snapshot(dir);
        for (ReplacePolicy pol : {ReplacePolicy::Forbid, ReplacePolicy::Allow}) {
            const DemDynaExportResult r = exportDemDyna(lawInput, dest, opts(pol));
            expectNotWritten(r);
        }
        CHECK(snapshot(dir) == before && tmpCount(dir) == 0);
        CHECK(readFile(dest) == std::optional<std::string>("arquivo anterior"));
    }
    {
        scenario("F. erro de validação constitutiva (tp = 3 em barra ativa)");
        const fs::path dir = g_root->sub("lei");
        const DemDynaExportResult r = exportDemDyna(lawInput, dir / L"x.txt");
        expectNotWritten(r);
        CHECK(r.composition.lawsValidated && r.composition.lawReport.blocksExport());
        bool tp3 = false;
        for (const dem::LawIssue& i : r.composition.lawReport.issues)
            tp3 = tp3 || (i.kind == dem::LawIssueKind::InvalidExported && i.tp == 3);
        CHECK(tp3);
        bool stageLeis = false;
        for (const DemDynaIssue& e : r.composition.errors)
            stageLeis = stageLeis || e.stage == "leis";
        CHECK(stageLeis && contains(r.message, "[leis]"));
        CHECK(snapshot(dir).empty());
        std::printf("    %s\n", r.message.c_str());
    }
    {
        scenario("G. colapso de coordenadas após %g (origem a 100 m)");
        const Discretization distant = caixa(100000.0);
        const fs::path dir = g_root->sub("colapso");
        const DemDynaExportResult r = exportDemDyna(simpleInput(distant), dir / L"x.txt", opts(ReplacePolicy::Allow));
        expectNotWritten(r);
        bool repr = false;
        for (const DemDynaIssue& e : r.composition.errors)
            repr = repr || e.stage == "representação APDL";
        CHECK(repr && !r.composition.lawsValidated);
        CHECK(snapshot(dir).empty());
        std::printf("    %s\n", r.message.c_str());
    }
    {
        scenario("N. outras recusas da composição: sem discretização, Gf ausente, barras excluídas");
        const fs::path dir = g_root->sub("entrada");
        const fs::path dest = dir / L"DEM_DYNA.txt";
        CHECK(writeRaw(dest, "anterior"));
        const auto before = snapshot(dir);
        const Discretization d = caixa();
        DemDynaInput noDisc = simpleInput(d);
        noDisc.discretization = nullptr;
        DemDynaInput noGf = simpleInput(d);
        noGf.gf.clear();
        DemDynaInput deleted = simpleInput(d);
        deleted.deleted.assign(deleted.gf.size(), false);
        deleted.deleted[0] = true;
        for (const DemDynaInput* in : {&noDisc, &noGf, &deleted}) {
            const DemDynaExportResult r = exportDemDyna(*in, dest, opts(ReplacePolicy::Allow));
            expectNotWritten(r);
        }
        CHECK(snapshot(dir) == before);
    }
}

void testWriteFailures(const std::string& ref)
{
    std::printf("[H, I, M] falhas da gravação com composição aceita\n");
    const Discretization d = caixa();
    const std::optional<DemDynaInput> in = referenceInput(d, ref);
    CHECK(in.has_value());
    if (!in)
        return;
    {
        scenario("H/M. destino bloqueado por outro programa [Allow]");
        const fs::path dir = g_root->sub("bloqueado");
        const fs::path dest = dir / L"DEM_DYNA.txt";
        CHECK(writeRaw(dest, "aberto no ANSYS"));
        HANDLE lock = ::CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE);
        const DemDynaExportResult r = exportDemDyna(*in, dest, opts(ReplacePolicy::Allow));
        ::CloseHandle(lock);
        show(r);
        CHECK(!r.ok && r.stage == DemDynaExportStage::Write && r.writeStarted);
        // M: composição sem erros e gravação com o próprio diagnóstico.
        CHECK(r.composition.ok && r.composition.errors.empty() && r.composition.text == ref);
        CHECK(r.write.stage == WriteStage::Publish && r.write.error == SafeWriteError::PublishFailed);
        CHECK(r.write.win32Error == ERROR_SHARING_VIOLATION || r.write.win32Error == ERROR_ACCESS_DENIED);
        CHECK(r.write.destinationPreserved && r.write.tempRemoved && r.write.cleanupWin32Error == 0);
        CHECK(r.bytesComposed == 58181 && r.bytesWritten == 58181 && r.bytesPublished == 0);
        CHECK(contains(r.message, "Publish/PublishFailed") && contains(r.message, "temporário removido"));
        CHECK(readFile(dest) == std::optional<std::string>("aberto no ANSYS") && tmpCount(dir) == 0);
        std::printf("    %s\n", r.message.c_str());
    }
    {
        scenario("I. cancelamento solicitado antes da publicação (antes do temporário)");
        const fs::path dir = g_root->sub("cancelado");
        const fs::path dest = dir / L"DEM_DYNA.txt";
        CHECK(writeRaw(dest, "anterior"));
        const auto before = snapshot(dir);
        std::atomic<bool> cancel{true};
        const DemDynaExportResult r = exportDemDyna(*in, dest, opts(ReplacePolicy::Allow, &cancel));
        show(r);
        CHECK(!r.ok && r.stage == DemDynaExportStage::Write && r.writeStarted);
        CHECK(r.composition.ok && r.write.error == SafeWriteError::Cancelled);
        CHECK(r.write.destinationPreserved && r.write.tempPath.empty() && r.bytesPublished == 0);
        CHECK(snapshot(dir) == before);
    }
    {
        scenario("M. diagnósticos separados: composição recusada x gravação recusada");
        const Discretization bad = ele();
        const fs::path dir = g_root->sub("separados");
        const DemDynaExportResult comp = exportDemDyna(simpleInput(bad), dir / L"a.txt");
        const DemDynaExportResult wr = exportDemDyna(*in, dir / L"inexistente" / L"a.txt");
        CHECK(comp.stage == DemDynaExportStage::Composition && !comp.composition.errors.empty() && !comp.writeStarted);
        CHECK(wr.stage == DemDynaExportStage::Write && wr.composition.errors.empty() && wr.writeStarted &&
              wr.write.error == SafeWriteError::DirectoryMissing);
        CHECK(snapshot(dir).empty());
    }
}

void testProtections(const std::string& ref)
{
    std::printf("[P] proteções adicionais\n");
    const Discretization d = caixa();
    const std::optional<DemDynaInput> in = referenceInput(d, ref);
    CHECK(in.has_value());
    if (!in)
        return;
    {
        scenario("entrada não modificada pela exportação (sucesso e falha)");
        const DemDynaInput copy = *in;
        const fs::path dir = g_root->sub("entrada_const");
        exportDemDyna(*in, dir / L"a.txt");
        exportDemDyna(*in, dir / L"a.txt"); // falha (Forbid)
        CHECK(in->discretization == copy.discretization && in->gf == copy.gf && in->deleted == copy.deleted);
        CHECK(in->header.seed == copy.header.seed && in->header.timestamp == copy.header.timestamp &&
              in->header.workingDirectory == copy.header.workingDirectory);
        CHECK(in->gfField.has_value() && in->gfField->rotationDegrees == copy.gfField->rotationDegrees);
        CHECK(in->properties == copy.properties);
        CHECK(composeDemDyna(*in).text == ref); // composição repetida continua idêntica
    }
    {
        scenario("caminhos inválidos nunca produzem sucesso (sem exceção)");
        const fs::path dir = g_root->sub("caminhos");
        const fs::path bad[] = {fs::path(), dir / L"CON", dir / L"a.txt:fluxo", dir / L"sem_dir" / L"a.txt",
                                fs::path(L"\\\\?\\") += (dir / L"a.txt").native(), dir / std::wstring(260, L'x'),
                                dir / std::wstring(L"a\0b.txt", 7)}; // NUL: dentro do diretório isolado
        int refused = 0;
        for (const fs::path& p : bad) {
            const DemDynaExportResult r = exportDemDyna(*in, p, opts(ReplacePolicy::Allow));
            const bool ok = !r.ok && r.stage == DemDynaExportStage::Write && r.writeStarted && !r.write.ok &&
                            r.bytesPublished == 0;
            refused += ok ? 1 : 0;
            CHECK(ok);
        }
        CHECK(snapshot(dir).empty());
        std::printf("    %d de %zu caminhos recusados\n", refused, sizeof bad / sizeof bad[0]);
    }
    {
        // E5.7.4b.8c.4d (A1/A2): NUL no nome e em componente de diretório, pela exportação completa. A
        // composição é aceita; o gravador recusa em PathCheck/UnsupportedPath; nada é publicado em
        // caminho algum (nem no prefixo truncado até o NUL).
        scenario("T08. NUL no caminho via exportDemDyna: composição válida, recusa do gravador");
        const fs::path dir = g_root->sub("nul_exportacao");
        const fs::path sentinel = dir / L"vitima.txt";
        const std::string original = "sentinela: conteúdo que não pode ser substituído\r\n";
        CHECK(writeRaw(sentinel, original));
        const std::wstring nul(1, L'\0');
        struct NulCase
        {
            const char* name;
            fs::path path;
            ReplacePolicy policy;
            fs::path truncated; // caminho até o NUL (onde a implementação anterior publicava)
        };
        const NulCase cases[] = {
            {"diretório, Allow sobre sentinela", dir / (L"vitima.txt" + nul + L"x") / L"DEM_DYNA.txt",
             ReplacePolicy::Allow, sentinel},
            {"diretório, Forbid (prefixo inexistente)", dir / (L"novo" + nul + L"x") / L"DEM_DYNA.txt",
             ReplacePolicy::Forbid, dir / L"novo"},
            {"nome final, Allow", dir / (L"DEM" + nul + L"_DYNA.txt"), ReplacePolicy::Allow, dir / L"DEM"},
        };
        for (const NulCase& c : cases) {
            std::printf("    %s\n", c.name);
            const std::size_t nulPos = c.path.native().find(L'\0');
            CHECK(nulPos != std::wstring::npos && c.path.native().substr(0, nulPos) == c.truncated.native());
            const auto before = snapshot(dir);
            const DemDynaExportResult r = exportDemDyna(*in, c.path, opts(c.policy));
            CHECK(r.composition.ok && r.composition.errors.empty() && r.bytesComposed == 58181);
            CHECK(!r.ok && r.stage == DemDynaExportStage::Write && r.writeStarted && !r.write.ok);
            CHECK(r.write.stage == WriteStage::PathCheck && r.write.error == SafeWriteError::UnsupportedPath);
            CHECK(r.write.tempPath.empty() && r.write.tempRemoved && r.write.destinationPreserved);
            CHECK(r.bytesWritten == 0 && r.bytesPublished == 0);
            CHECK(r.message.find('\0') == std::string::npos && r.write.message.find('\0') == std::string::npos);
            CHECK(contains(r.write.message, "caractere de controle \\x00"));
            CHECK(snapshot(dir) == before); // sentinela: mesmo tamanho, data e índice; nada criado
            CHECK(readFile(sentinel) == std::optional<std::string>(original));
            if (c.truncated != sentinel)
                CHECK(!pathExists(c.truncated)); // nenhuma publicação no caminho alternativo
        }
        CHECK(snapshot(dir).size() == 1 && tmpCount(dir) == 0);
    }
    {
        scenario("política inválida não é corrigida pela camada");
        const fs::path dir = g_root->sub("politica");
        const DemDynaExportResult r = exportDemDyna(*in, dir / L"a.txt", opts(static_cast<ReplacePolicy>(9)));
        CHECK(!r.ok && r.writeStarted && r.write.error == SafeWriteError::InvalidOptions);
        CHECK(SafeWriteOptions{}.replace == ReplacePolicy::Forbid);
        CHECK(snapshot(dir).empty());
    }
}


// ---------------------------------------------------------------------------------------------
// Q. Falhas de alocação injetadas em exportDemDyna (E5.7.4b.8c.4b). Cada execução usa um
// subdiretório novo (destino inexistente, Forbid) e verifica, qualquer que seja a alocação que falhe:
// nenhuma exceção escapa; ok/stage/write.ok/bytesPublished coerentes; arquivo publicado nunca
// descrito como não publicado; garantias de preservação/limpeza nunca presumidas; nenhuma segunda
// gravação (DestinationExists nunca aparece, pois o destino estava ausente).
struct OomTally
{
    int runs = 0, escaped = 0, success = 0, successMsgFallback = 0, notStartedInternal = 0, beforeWrite = 0,
        unconfirmed = 0, writerFailed = 0, publishedThenFailed = 0, emptyMessage = 0;
};

void checkOomRun(const DemDynaExportResult& r, bool escaped, const fs::path& dir, const fs::path& dest,
                 const std::string& ref, OomTally& t)
{
    ++t.runs;
    CHECK(!escaped);
    if (escaped) {
        ++t.escaped;
        return;
    }
    const auto files = snapshot(dir);
    const bool destExists = files.count(dest.filename().native()) != 0;
    const std::size_t tmps = tmpCount(dir);
    if (r.message.empty())
        ++t.emptyMessage;

    // Coerência geral.
    CHECK(r.ok == (r.stage == DemDynaExportStage::Done));
    CHECK(!r.ok || (r.write.ok && r.writeStarted && r.composition.ok));
    CHECK(r.ok ? r.bytesPublished == 58181 : r.bytesPublished == 0);
    CHECK(r.write.error != SafeWriteError::DestinationExists); // nenhuma segunda tentativa
    CHECK(destExists || !r.write.ok);
    CHECK(tmps == 0 || !r.write.tempRemoved); // temporário restante nunca negado
    if (destExists)
        CHECK(readFile(dest) == std::optional<std::string>(ref)); // publicado = bytes verificados

    if (!r.writeStarted) {
        // P2: gravação explicitamente não iniciada.
        CHECK(r.stage == DemDynaExportStage::Composition && !r.composition.ok && r.composition.text.empty());
        CHECK(!r.write.ok && r.write.error == SafeWriteError::None && r.write.bytes == 0 && r.write.tempPath.empty());
        CHECK(r.write.tempRemoved && r.write.destinationPreserved); // por construção (não chamada)
        CHECK(r.bytesComposed == 0 && r.bytesWritten == 0);
        CHECK(files.empty());
        CHECK(r.message.empty() || contains(r.message, "nenhum arquivo foi gravado") || r.message == "não gravado");
        if (contains(r.message, "erro interno") ||
            (!r.composition.errors.empty() && contains(r.composition.errors.front().message, "erro interno")))
            ++t.notStartedInternal;
        if (contains(r.message, "antes da gravação"))
            ++t.beforeWrite;
        return;
    }
    CHECK(r.composition.ok && r.bytesComposed == 58181 && r.stage != DemDynaExportStage::Composition);
    if (r.ok) {
        CHECK(destExists && tmps == 0 && r.write.stage == WriteStage::Done);
        CHECK(r.message.empty() || contains(r.message, "concluída") || r.message == "publicado");
        ++t.success;
        if (contains(r.message, "mensagem detalhada indisponível") || r.message == "publicado")
            ++t.successMsgFallback;
        return;
    }
    CHECK(r.stage == DemDynaExportStage::Write);
    const bool unconfirmed = r.write.error == SafeWriteError::Internal && r.write.tempPath.empty() &&
                             r.write.bytes == 0 && !r.write.destinationPreserved && !r.write.tempRemoved;
    if (unconfirmed) {
        // P3: writeFileSafely lançou; nada é presumido.
        ++t.unconfirmed;
        CHECK(r.message.empty() || contains(r.message, "não confirmado"));
    } else if (r.write.stage == WriteStage::Done) {
        // Gravador publicou e falhou depois: a mensagem não nega a publicação.
        ++t.publishedThenFailed;
        CHECK(destExists && r.write.error == SafeWriteError::Internal);
        CHECK(r.message.empty() || contains(r.message, "DEPOIS da publicação") || r.message == "publicado");
    } else {
        // Falha relatada pelo próprio gravador antes da publicação.
        ++t.writerFailed;
        CHECK(!destExists);
        CHECK(!r.write.destinationPreserved || !destExists);
    }
}

void testAllocationFailures(const std::string& ref)
{
    std::printf("[Q] falhas de alocação injetadas (P1-P3)\n");
    const Discretization d = caixa();
    const std::optional<DemDynaInput> in = referenceInput(d, ref);
    CHECK(in.has_value());
    if (!in)
        return;
    const fs::path base = g_root->sub("oom");
    int seq = 0;
    auto runOnce = [&](long long failAt, bool persistent, OomTally& t) {
        const fs::path dir = base / std::to_wstring(++seq);
        CHECK(::CreateDirectoryW(dir.c_str(), nullptr) != 0);
        const fs::path dest = dir / L"DEM_DYNA.txt";
        DemDynaExportResult r;
        bool escaped = false;
        oom::arm(failAt, persistent);
        try {
            r = exportDemDyna(*in, dest);
        } catch (...) {
            escaped = true;
        }
        const long long count = oom::disarm();
        checkOomRun(r, escaped, dir, dest, ref, t);
        removeTree(dir);
        return count;
    };

    // Contagens de referência (sem falhas).
    oom::arm(0, false);
    const DemDynaComposition probe = composeDemDyna(*in);
    const long long nCompose = oom::disarm();
    CHECK(probe.ok && probe.text == ref);
    OomTally clean;
    const long long nTotal = runOnce(0, false, clean);
    CHECK(clean.success == 1);
    std::printf("    alocações: composição %lld, exportação completa %lld\n", nCompose, nTotal);

    // Pontos: início exaustivo, composição amostrada, gravação e mensagens exaustivas.
    std::vector<long long> points;
    const long long head = std::min<long long>(40, nTotal);
    for (long long k = 1; k <= head; ++k)
        points.push_back(k);
    const long long tailStart = std::max<long long>(head + 1, nCompose - 40);
    for (int i = 1; i <= 48; ++i) {
        const long long k = head + (tailStart - head) * i / 49;
        if (k > head && k < tailStart && k != points.back())
            points.push_back(k);
    }
    for (long long k = tailStart; k <= nTotal + 2; ++k)
        points.push_back(k);

    OomTally all;
    for (const bool persistent : {false, true}) {
        scenario(persistent ? "Q. falha persistente a partir da n-ésima alocação (pouca memória)"
                            : "Q. falha isolada na n-ésima alocação");
        OomTally t;
        for (const long long k : points)
            runOnce(k, persistent, t);
        std::printf("    execuções %d: sucesso %d (mensagem de reserva %d), não iniciada por erro interno %d "
                    "(antes da gravação %d), não confirmada %d, falha do gravador %d, publicada e depois falha %d, "
                    "mensagem vazia %d, exceções escapadas %d\n",
                    t.runs, t.success, t.successMsgFallback, t.notStartedInternal, t.beforeWrite, t.unconfirmed,
                    t.writerFailed, t.publishedThenFailed, t.emptyMessage, t.escaped);
        CHECK(t.escaped == 0);
        all.escaped += t.escaped;
        all.beforeWrite += t.beforeWrite;
        all.unconfirmed += t.unconfirmed;
        all.successMsgFallback += t.successMsgFallback;
        all.publishedThenFailed += t.publishedThenFailed;
    }
    // Cobertura (nos dois modos somados). O diagnóstico detalhado "antes da gravação" só cabe na
    // mensagem quando ainda há memória (modo isolado).
    // E5.7.4b.8c.4c: o gravador corrigido não lança mais (o próprio tratamento de erro dele não aloca
    // fora de try) e nunca devolve stage = Done com ok = false. Na E5.7.4b.8c.4b esses dois estados
    // eram alcançados SOMENTE por causa desse defeito do gravador; agora devem estar ausentes. O
    // tratamento P3 (writeThrew) e o ramo "publicado e depois falhou" continuam no exportador como
    // mecanismos defensivos, sem caminho de alocação que os alcance.
    CHECK(all.beforeWrite > 0);          // P2 exercitado
    CHECK(all.successMsgFallback > 0);   // P1 exercitado
    CHECK(all.unconfirmed == 0);         // writeFileSafely não lançou
    CHECK(all.publishedThenFailed == 0); // publicação nunca seguida de "falha" do gravador
    CHECK(snapshot(base).empty());

    // H (E5.7.4b.8c.4c): destino EXISTENTE com Allow — a cada falha injetada, o conteúdo real do disco
    // decide: arquivo publicado ⇔ ok (nunca descrito como não publicado), bytesPublished coerente e,
    // sem publicação, o arquivo anterior intacto.
    const std::string original = "conteúdo anterior do destino\r\n";
    SafeWriteOptions allow;
    allow.replace = ReplacePolicy::Allow;
    OomTally rep;
    int repPublished = 0;
    for (const bool persistent : {false, true}) {
        scenario(persistent ? "Q-H. destino existente/Allow: falha persistente a partir da n-ésima alocação"
                            : "Q-H. destino existente/Allow: falha isolada na n-ésima alocação");
        for (const long long k : points) {
            const fs::path dir = base / std::to_wstring(++seq);
            CHECK(::CreateDirectoryW(dir.c_str(), nullptr) != 0);
            const fs::path dest = dir / L"DEM_DYNA.txt";
            CHECK(writeRaw(dest, original));
            DemDynaExportResult r;
            bool escaped = false;
            oom::arm(k, persistent);
            try {
                r = exportDemDyna(*in, dest, allow);
            } catch (...) {
                escaped = true;
            }
            oom::disarm();
            ++rep.runs;
            CHECK(!escaped);
            if (escaped) {
                ++rep.escaped;
                removeTree(dir);
                continue;
            }
            const auto now = readFile(dest);
            const bool published = now.has_value() && *now == ref;
            CHECK(published || (now.has_value() && *now == original));
            CHECK(r.ok == published); // publicação efetiva nunca descrita como inexistente
            CHECK(r.ok == (r.stage == DemDynaExportStage::Done));
            CHECK(r.ok ? r.bytesPublished == 58181 : r.bytesPublished == 0);
            CHECK(!r.ok || (r.write.ok && r.writeStarted && r.composition.ok && r.bytesWritten == 58181));
            CHECK(r.write.ok || r.write.stage != WriteStage::Done);
            CHECK(tmpCount(dir) == 0 || !r.write.tempRemoved);
            if (published) {
                ++repPublished;
                CHECK(r.write.destinationExisted && r.write.destinationReplaced && !r.write.destinationPreserved);
                if (contains(r.message, "mensagem detalhada indisponível") || r.message == "publicado")
                    ++rep.successMsgFallback;
            } else if (r.writeStarted) {
                CHECK(r.stage == DemDynaExportStage::Write);
                CHECK(r.write.destinationPreserved && !r.write.destinationReplaced);
                if (r.write.error == SafeWriteError::Internal && r.write.tempPath.empty() &&
                    !r.write.destinationPreserved)
                    ++rep.unconfirmed;
            } else {
                CHECK(r.stage == DemDynaExportStage::Composition && r.bytesComposed == 0 && r.bytesWritten == 0);
            }
            removeTree(dir);
        }
    }
    std::printf("    destino existente/Allow: execuções %d, publicadas %d (mensagem de reserva %d), "
                "exceções escapadas %d\n",
                rep.runs, repPublished, rep.successMsgFallback, rep.escaped);
    CHECK(rep.escaped == 0 && rep.unconfirmed == 0);
    CHECK(repPublished > 0 && repPublished < rep.runs);
    CHECK(snapshot(base).empty());
}

} // namespace

int main()
{
    std::printf("E5.7.4b.8c.4 - exportação DEM-DYNA (composeDemDyna + writeFileSafely)\n");
    const auto ref = readFile(fs::u8path(CAD_MATLAB_REFERENCE_DIR "/DEM_DYNA.txt"));
    CHECK(ref.has_value() && ref->size() == 58181); // referência obrigatória (somente leitura)
    fs::path rootPath;
    {
        TestRoot root;
        CHECK(root.valid());
        if (ref && root.valid()) {
            g_root = &root;
            rootPath = root.path();
            testValid(*ref);
            testInvalid();
            testWriteFailures(*ref);
            testProtections(*ref);
            testAllocationFailures(*ref);
            g_root = nullptr;
        }
    }
    CHECK(rootPath.empty() || !pathExists(rootPath));
    std::printf("\ncenários: %d; verificações: %d; diretório isolado removido: %s\n", g_scenarios, g_checks,
                rootPath.empty() || !pathExists(rootPath) ? "sim" : "NAO");
    return cad::test::finish("e574b8c4");
}
