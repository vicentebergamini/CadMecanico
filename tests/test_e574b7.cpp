// E5.7.4b.7 sem interface — validação das leis constitutivas e das associações elemento–material
// antes da exportação APDL/DYNA (dem/LawValidation.h):
//  [A] DEM_DYNA.txt: 51 leis, 490 barras, nenhum bloqueio, associações e texto preservados;
//  [B] lei tp = 3 atribuída a barra ativa: InvalidExported com IDs da lei e do elemento;
//  [C] lei tp = 3 só em barras excluídas: ReferenceZero + verificação dos comandos APDL escritos;
//  [D] kr ≤ 1: abscissas não crescentes (e o caso de kr pouco maior que 1 após o %g);
//  [E] EEP, FP, EEU nulos, negativos ou não finitos;
//  [F] IDs inválidos e associações inconsistentes: bloqueio estrutural;
//  [G] comprimentos diferentes na mesma lei: aviso informativo, valores MATLAB inalterados;
//  [H] vetor deleted vazio = nenhuma barra excluída;
//  [I] Inf e NaN: diagnóstico específico; o texto que seria escrito é inválido e a exportação bloqueia;
//  [J] kr não finito (Li subnormal): origem da lei (elemento que a criou) × elemento afetado, escopos
//      barra / lei e bloco APDL sem mensagens contraditórias, InvalidEmitted preservado.
#include "TestCheck.h"

#include "dem/LatticeConstants.h"
#include "dem/LawValidation.h"
#include "dem/MatlabLawTable.h"
#include "discretization/ApdlExporter.h" // linkLengths (somente leitura; o exportador não é alterado)
#include "discretization/LatticeTopology.h"
#include "discretization/MatlabLawApdl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace cad::discretization;
namespace dem = cad::dem;

namespace {

const double kInf = std::numeric_limits<double>::infinity();
const double kNaN = std::numeric_limits<double>::quiet_NaN();

dem::LatticeConstants lattice()
{
    return dem::computeLattice(dem::matlabDefaults(), 1e-3);
}

dem::MatlabLawTable table(const std::vector<std::int8_t>& tp, const std::vector<double>& li,
                          const std::vector<double>& gf)
{
    return dem::buildMatlabLawTable(lattice(), tp, li, gf);
}

bool bitsEqual(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

// Tabela idêntica bit a bit (a validação não altera nada).
bool sameTable(const dem::MatlabLawTable& a, const dem::MatlabLawTable& b)
{
    if (a.ok != b.ok || a.curveOffset != b.curveOffset || a.bondLaw != b.bondLaw || a.laws.size() != b.laws.size())
        return false;
    if (!bitsEqual(a.bondKr, b.bondKr) || !bitsEqual(a.bondEep, b.bondEep) || !bitsEqual(a.bondFp, b.bondFp)
        || !bitsEqual(a.bondEeu, b.bondEeu))
        return false;
    for (std::size_t j = 0; j < a.laws.size(); ++j)
    {
        const dem::MatlabLaw &x = a.laws[j], &y = b.laws[j];
        const double vx[] = {x.eep, x.fp, x.eeu, x.kr, x.li, x.gf}, vy[] = {y.eep, y.fp, y.eeu, y.kr, y.li, y.gf};
        if (std::memcmp(vx, vy, sizeof vx) != 0 || x.id != y.id || x.tp != y.tp || x.firstBond != y.firstBond
            || std::memcmp(&x.loading, &y.loading, sizeof x.loading) != 0
            || std::memcmp(&x.unloading, &y.unloading, sizeof x.unloading) != 0)
            return false;
    }
    return true;
}

// Primeiro diagnóstico com os campos pedidos (nullptr se não houver).
const dem::LawIssue* find(const dem::LawValidationReport& r, dem::LawIssueKind kind, std::int32_t lawId,
                          const std::string& parameterPrefix = "")
{
    for (const dem::LawIssue& i : r.issues)
        if (i.kind == kind && i.lawId == lawId && i.parameter.rfind(parameterPrefix, 0) == 0)
            return &i;
    return nullptr;
}

void show(const char* label, const dem::LawValidationReport& r, std::size_t max = 4)
{
    std::printf("  %s: %zu diagnostico(s), bloqueia = %s\n", label, r.issues.size(), r.blocksExport() ? "sim" : "nao");
    for (std::size_t i = 0; i < r.issues.size() && i < max; ++i)
        std::printf("    %s\n", dem::describe(r.issues[i]).c_str());
}

std::string readAll(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Abscissas escritas no texto APDL: linha "d<c><j>(1)= a,b,c,d,e". true se estritamente crescentes.
bool writtenAbscissasIncreasing(const std::string& text, int curve, int lawId)
{
    const std::string tag = "\nd" + std::to_string(curve) + std::to_string(lawId) + "(1)= ";
    const std::size_t p = text.find(tag);
    if (p == std::string::npos)
        return false;
    const char* s = text.c_str() + p + tag.size();
    double x[5];
    for (int i = 0; i < 5; ++i)
    {
        char* end = nullptr;
        x[i] = std::strtod(s, &end);
        s = end + 1; // vírgula
    }
    for (int i = 0; i < 4; ++i)
        if (!(x[i] < x[i + 1]))
            return false;
    return true;
}

// ------------------------------------------------------------------ [A]
void testDemDyna(const char* path)
{
    std::printf("[A] DEM_DYNA.txt: 51 leis, 490 barras, sem bloqueio\n");
    const std::string file = readAll(path);
    CAD_CHECK(!file.empty());
    if (file.empty())
        return;
    const std::size_t lawStart = file.find("\n\n!************************INICIO DA LEI 1\n");
    const std::size_t define = file.find("! ***********************DEFINE A LEI EM CADA MOLA e ENERGIAS\n");
    const std::size_t esel = file.find("ESEL,S,TYPE,,1\n");
    CAD_CHECK(lawStart != std::string::npos && define != std::string::npos && esel != std::string::npos);
    if (lawStart == std::string::npos || define == std::string::npos || esel == std::string::npos)
        return;
    const std::string refLaws = file.substr(lawStart + 1, define - lawStart - 1);
    const std::string refEmodif = file.substr(define, esel - define);

    // Mesma reconstrução da tabela de test_e574b6 (tp e Li da discretização C++, Gf recuperado de FP e
    // ajustado à classe xm).
    VoxelGrid g = VoxelGrid::make(gp_XYZ(0.0, 0.0, 0.0), 1.0);
    g.nx = 4;
    g.ny = 4;
    g.nz = 2;
    Parameters p;
    p.lco = 1.0;
    p.criterion = InclusionCriterion::CenterInside;
    Discretization d;
    CAD_CHECK(d.build(g, p, std::vector<std::uint8_t>(32, 1)));
    const LatticeTopology topo = classifyLattice(d);
    const std::vector<double> len = linkLengths(d, LengthUnit::Meter);
    const dem::LatticeConstants c = lattice();
    const dem::WeibullClasses w = dem::weibullBins(0.8, 20);
    std::vector<double> fpOfLaw(1, 0.0);
    std::size_t q = 0;
    while ((q = refLaws.find("TBDAT,4,", q)) != std::string::npos)
        fpOfLaw.push_back(std::strtod(refLaws.c_str() + (q += 8), nullptr));
    std::vector<int> lawOfBond;
    q = 0;
    while ((q = refEmodif.find(",MAT,", q)) != std::string::npos)
        lawOfBond.push_back(int(std::strtod(refEmodif.c_str() + (q += 5), nullptr)));
    CAD_CHECK(fpOfLaw.size() == 52 && lawOfBond.size() == 490 && topo.links() == 490);
    if (fpOfLaw.size() != 52 || lawOfBond.size() != 490 || topo.links() != 490)
        return;
    std::vector<double> gf(490);
    for (std::size_t b = 0; b < 490; ++b)
    {
        const int tp = topo.linkTp[b];
        const double ea = tp == 1 ? c.enr / 4.0 : tp == 2 ? c.enr / 2.0 : tp == 4 ? c.enr : c.edg;
        const double approx = fpOfLaw[std::size_t(lawOfBond[b])] / (ea * c.epr);
        std::size_t best = 0;
        for (std::size_t i = 1; i < w.xm.size(); ++i)
            if (std::fabs(w.xm[i] - approx) < std::fabs(w.xm[best] - approx))
                best = i;
        gf[b] = w.xm[best];
    }
    const dem::MatlabLawTable t = dem::buildMatlabLawTable(c, topo.linkTp, len, gf);
    const dem::MatlabLawTable copy = t;
    CAD_CHECK(t.ok && t.lawCount() == 51 && t.bonds() == 490);

    const dem::LawValidationReport r = dem::validateLawTable(t, {});
    const dem::LawValidationReport r2 = dem::validateLawTable(t, std::vector<bool>(490, false));
    CAD_CHECK(!r.blocksExport() && !r2.blocksExport());
    CAD_CHECK(r.issues.empty() && r2.issues.empty());
    CAD_CHECK(r.laws == 51 && r.bonds == 490 && r.deletedBonds == 0 && r.lawsOnlyDeleted == 0);
    CAD_CHECK(r.invalidLaws.empty() && r.blockedElements.empty());
    CAD_CHECK(sameTable(t, copy));
    bool assoc = true;
    for (std::size_t b = 0; b < 490; ++b)
        assoc = assoc && t.bondLaw[b] == lawOfBond[b];
    CAD_CHECK(assoc);
    const bool textSame = matlabLawBlock(t) == refLaws && matlabEmodifBlock(t) == refEmodif;
    CAD_CHECK(textSame);
    std::printf("  leis = %d  barras = %lld  diagnosticos = %zu  bloqueia = %s  associacoes 490/490 %s  texto %s\n",
                r.laws, (long long)r.bonds, r.issues.size(), r.blocksExport() ? "sim" : "nao",
                assoc ? "preservadas" : "DIFERENTES", textSame ? "identico ao DEM_DYNA.txt" : "DIFERENTE");
}

// ------------------------------------------------------------------ [B] e [C]
// Caso tp3_nula da E5.7.4b.6: tp = [3 4 3 1]; lei 1 = tp 3 (elementos 1 e 3), lei 2 = tp 4, lei 3 = tp 1.
dem::MatlabLawTable tp3Table()
{
    return table({3, 4, 3, 1}, {1e-3, 1e-3, 1e-3, 1e-3}, {1, 1, 1, 1});
}

void testTp3Active()
{
    std::printf("[B] lei tp = 3 atribuida a barra ativa\n");
    const dem::MatlabLawTable t = tp3Table();
    const dem::MatlabLawTable copy = t;
    CAD_CHECK(t.ok && t.lawCount() == 3 && t.laws[0].tp == 3 && t.laws[0].fp == 0.0);
    const dem::LawValidationReport r = dem::validateLawTable(t, {});
    CAD_CHECK(r.blocksExport());
    const dem::LawIssue* i = find(r, dem::LawIssueKind::InvalidExported, 1, "FP");
    CAD_CHECK(i != nullptr);
    if (i)
    {
        CAD_CHECK(i->elementId == 1 && i->tp == 3 && i->value == 0.0 && i->activeElements == 2);
        CAD_CHECK(i->category == dem::LawIssueCategory::Constitutive && i->blocksExport());
        CAD_CHECK(i->severity() == dem::LawIssueSeverity::Error);
        const std::string s = dem::describe(*i);
        CAD_CHECK(s.find("lei 1") != std::string::npos && s.find("elemento 1") != std::string::npos);
    }
    CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, 1, "d11(") != nullptr);  // abscissas nulas, carga
    CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, 1, "d21(") != nullptr);  // descarga
    CAD_CHECK(find(r, dem::LawIssueKind::ReferenceZero, 1) == nullptr);
    CAD_CHECK(r.invalidLaws == std::vector<std::int32_t>{1});
    CAD_CHECK((r.blockedElements == std::vector<std::int64_t>{1, 3}));
    CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, 2) == nullptr && find(r, dem::LawIssueKind::InvalidExported, 3) == nullptr);
    CAD_CHECK(sameTable(t, copy) && t.laws[0].eep == 0.0 && t.laws[0].eeu == 0.0); // 3·ENR/4 não aplicado
    show("tp = 3 em barras ativas", r);

    // Elemento 1 excluído, elemento 3 ativo: o diagnóstico aponta o elemento ativo.
    const dem::LawValidationReport r1 = dem::validateLawTable(t, {true, false, false, false});
    const dem::LawIssue* i1 = find(r1, dem::LawIssueKind::InvalidExported, 1, "FP");
    CAD_CHECK(i1 != nullptr && i1->elementId == 3 && i1->activeElements == 1);
    CAD_CHECK((r1.blockedElements == std::vector<std::int64_t>{3}));
}

void testTp3OnlyDeleted()
{
    std::printf("[C] lei tp = 3 somente em barras excluidas\n");
    const dem::MatlabLawTable t = tp3Table();
    const dem::LawValidationReport r = dem::validateLawTable(t, {true, false, true, false});
    CAD_CHECK(r.deletedBonds == 2 && r.lawsOnlyDeleted == 1);
    const dem::LawIssue* z = find(r, dem::LawIssueKind::ReferenceZero, 1, "FP");
    CAD_CHECK(z != nullptr);
    if (z)
        CAD_CHECK(z->elementId == 1 && z->tp == 3 && !z->blocksExport() && z->severity() == dem::LawIssueSeverity::Info);
    CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, 1) == nullptr);
    CAD_CHECK(r.blockedElements.empty());

    // Comandos APDL efetivamente escritos (todas as leis, como lawnumbers.m): as curvas da lei 1
    // continuam no arquivo com abscissas (−0, 0, 0, 0, 0), não crescentes.
    const std::string text = matlabLawBlock(t) + matlabEmodifBlock(t);
    CAD_CHECK(text.find("d11(1)= -0,0,0,0,0\nEDCURVE,ADD,1,d11(1),f11(1)\n") != std::string::npos);
    CAD_CHECK(text.find("d21(1)= -0,0,0,0,0\nEDCURVE,ADD,") != std::string::npos);
    CAD_CHECK(text.find("TBDAT,4,0 !TYI \nTBDAT,5,-0 !CYI \n") != std::string::npos);
    CAD_CHECK(text.find("EMODIF,1.000000,MAT,1.000000, \n") != std::string::npos);
    CAD_CHECK(!writtenAbscissasIncreasing(text, 1, 1) && !writtenAbscissasIncreasing(text, 2, 1));
    CAD_CHECK(writtenAbscissasIncreasing(text, 1, 2) && writtenAbscissasIncreasing(text, 2, 3));
    // A validação acusa o mesmo problema dos comandos escritos: InvalidEmitted (bloqueante).
    const dem::LawIssue* e1 = find(r, dem::LawIssueKind::InvalidEmitted, 1, "d11(");
    const dem::LawIssue* e2 = find(r, dem::LawIssueKind::InvalidEmitted, 1, "d21(");
    CAD_CHECK(e1 != nullptr && e2 != nullptr);
    if (e1)
        CAD_CHECK(e1->elementId == 1 && e1->activeElements == 0 && e1->blocksExport());
    CAD_CHECK(r.blocksExport()); // por causa dos comandos escritos, não do ReferenceZero
    CAD_CHECK(r.invalidLaws == std::vector<std::int32_t>{1});
    show("tp = 3 so em barras excluidas", r);
    std::printf("  comandos escritos da lei 1: d11(1)= -0,0,0,0,0 / d21(1)= -0,0,0,0,0 -> abscissas nao crescentes\n");
}

// ------------------------------------------------------------------ [D]
void testKr()
{
    std::printf("[D] kr <= 1: abscissas nao crescentes\n");
    const double lcr = lattice().lcr;
    // Li = 2·LCR (kr = 0,5) e Li = LCR (kr = 1), tp = 4, Gf distintos (duas leis).
    const dem::MatlabLawTable t = table({4, 4}, {2.0 * lcr, lcr}, {1.0, 1.1});
    CAD_CHECK(t.ok && t.lawCount() == 2 && t.laws[0].kr == 0.5 && t.laws[1].kr == 1.0);
    const dem::LawValidationReport r = dem::validateLawTable(t, {});
    CAD_CHECK(r.blocksExport());
    for (std::int32_t law : {1, 2})
    {
        const dem::LawIssue* i = find(r, dem::LawIssueKind::InvalidExported, law, "d1" + std::to_string(law) + "(");
        CAD_CHECK(i != nullptr);
        if (i)
            CAD_CHECK(i->category == dem::LawIssueCategory::Constitutive && i->elementId == law && i->tp == 4
                      && i->parameter == "d1" + std::to_string(law) + "(4)");
        CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, law, "d2" + std::to_string(law) + "(") != nullptr);
        CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, law, "EEP") == nullptr); // EEP, FP, EEU > 0
    }
    show("kr = 0,5 e kr = 1", r, 2);

    // kr pouco maior que 1: crescente em precisão dupla, mas δp e δr iguais no texto (%g).
    const double li = lcr / (1.0 + 1e-9);
    const dem::MatlabLawTable t2 = table({4}, {li}, {1.0});
    const dem::MatlabLaw& w = t2.laws[0];
    const bool rawIncreasing = w.loading.displacement[2] < w.loading.displacement[3];
    CAD_CHECK(w.kr > 1.0 && rawIncreasing);
    CAD_CHECK(matlabG(w.loading.displacement[2]) == matlabG(w.loading.displacement[3]));
    const dem::LawValidationReport r2 = dem::validateLawTable(t2, {});
    const dem::LawIssue* n = find(r2, dem::LawIssueKind::InvalidExported, 1, "d11(4)");
    CAD_CHECK(n != nullptr && n->category == dem::LawIssueCategory::Numeric && r2.blocksExport());
    show("kr = 1 + 1e-9 (precisao do %g)", r2, 1);
    // Excluída: o comando continua escrito, InvalidEmitted.
    const dem::LawValidationReport r3 = dem::validateLawTable(t, {true, true});
    CAD_CHECK(r3.blocksExport() && r3.blockedElements.empty() && r3.count(dem::LawIssueKind::InvalidExported) == 0);
    CAD_CHECK(find(r3, dem::LawIssueKind::InvalidEmitted, 1, "d11(4)") != nullptr);
}

// ------------------------------------------------------------------ [E] e [I]
void testValues()
{
    std::printf("[E] EEP, FP, EEU nulos, negativos ou nao finitos\n");
    // Gf = 0 (tp = 4, lei 1) e Gf = −1 (tp = 0, lei 2), lei 3 válida.
    const dem::MatlabLawTable t = table({4, 0, 4}, {1e-3, 1e-3 * std::sqrt(3.0) / 2.0, 1e-3}, {0.0, -1.0, 1.0});
    CAD_CHECK(t.ok && t.lawCount() == 3);
    const dem::LawValidationReport r = dem::validateLawTable(t, {});
    CAD_CHECK(r.blocksExport());
    for (const char* p : {"EEP", "FP", "EEU"})
    {
        const dem::LawIssue* a = find(r, dem::LawIssueKind::InvalidExported, 1, p);
        const dem::LawIssue* b = find(r, dem::LawIssueKind::InvalidExported, 2, p);
        CAD_CHECK(a != nullptr && a->value == 0.0 && a->category == dem::LawIssueCategory::Constitutive);
        CAD_CHECK(b != nullptr && b->value < 0.0 && b->elementId == 2 && b->tp == 0);
    }
    CAD_CHECK(find(r, dem::LawIssueKind::InvalidExported, 3) == nullptr);
    CAD_CHECK((r.invalidLaws == std::vector<std::int32_t>{1, 2}));
    CAD_CHECK((r.blockedElements == std::vector<std::int64_t>{1, 2}));
    show("Gf = 0 e Gf = -1", r, 3);
    // Somente a barra inválida excluída: os comandos continuam escritos (InvalidEmitted).
    const dem::LawValidationReport rd = dem::validateLawTable(t, {true, true, false});
    CAD_CHECK(rd.count(dem::LawIssueKind::InvalidExported) == 0 && rd.blockedElements.empty() && rd.blocksExport());
    CAD_CHECK(find(rd, dem::LawIssueKind::InvalidEmitted, 1, "FP") != nullptr);

    std::printf("[I] valores Inf e NaN\n");
    const dem::MatlabLawTable base = table({4, 4}, {1e-3, 1e-3}, {1.0, 1.2});
    // FP = Inf na lei 1 (e nas barras dela, mantendo a tabela coerente); curva com o mesmo valor.
    dem::MatlabLawTable inf = base;
    inf.laws[0].fp = kInf;
    inf.bondFp[0] = kInf;
    inf.laws[0].loading.force[2] = kInf;
    const dem::LawValidationReport ri = dem::validateLawTable(inf, {});
    const dem::LawIssue* fi = find(ri, dem::LawIssueKind::InvalidExported, 1, "FP");
    CAD_CHECK(fi != nullptr && std::isinf(fi->value) && fi->category == dem::LawIssueCategory::Numeric);
    if (fi)
        CAD_CHECK(dem::describe(*fi).find("FP = Inf") != std::string::npos);
    CAD_CHECK(find(ri, dem::LawIssueKind::InvalidExported, 1, "f11(3)") != nullptr);
    CAD_CHECK(find(ri, dem::LawIssueKind::InvalidExported, 2) == nullptr);
    const std::string textInf = matlabLawBlock(inf);
    CAD_CHECK(textInf.find("inf") != std::string::npos && ri.blocksExport()); // texto inválido não pode ser escrito
    // EEP = NaN e abscissa NaN.
    dem::MatlabLawTable nan = base;
    nan.laws[1].eep = kNaN;
    nan.bondEep[1] = kNaN;
    nan.laws[1].unloading.displacement[4] = kNaN;
    const dem::LawValidationReport rn = dem::validateLawTable(nan, {});
    const dem::LawIssue* ni = find(rn, dem::LawIssueKind::InvalidExported, 2, "EEP");
    CAD_CHECK(ni != nullptr && std::isnan(ni->value) && ni->category == dem::LawIssueCategory::Numeric);
    if (ni)
        CAD_CHECK(dem::describe(*ni).find("EEP = NaN") != std::string::npos && ni->elementId == 2);
    CAD_CHECK(find(rn, dem::LawIssueKind::InvalidExported, 2, "d22(5)") != nullptr);
    CAD_CHECK(matlabLawBlock(nan).find("nan") != std::string::npos && rn.blocksExport());
    CAD_CHECK(dem::formatIssueValue(-kInf) == "-Inf" && dem::formatIssueValue(kNaN) == "NaN");
    show("FP = Inf", ri, 2);
    show("EEP = NaN", rn, 2);
}

// ------------------------------------------------------------------ [F]
void testStructural()
{
    std::printf("[F] IDs invalidos e associacoes inconsistentes\n");
    const dem::MatlabLawTable base = table({4, 4, 1, 4}, {1e-3, 1e-3, 1e-3, 1e-3}, {1.0, 1.2, 1.0, 1.0});
    CAD_CHECK(base.ok && base.lawCount() == 3 && !dem::validateLawTable(base, {}).blocksExport());
    const auto structural = [](const dem::LawValidationReport& r, const char* parameter) {
        for (const dem::LawIssue& i : r.issues)
            if (i.category == dem::LawIssueCategory::Structural && i.blocksExport() && i.parameter == parameter)
                return &i;
        return static_cast<const dem::LawIssue*>(nullptr);
    };

    dem::MatlabLawTable a = base; // referência a lei inexistente
    a.bondLaw[3] = 9;
    const dem::LawValidationReport ra = dem::validateLawTable(a, {});
    const dem::LawIssue* ia = structural(ra, "mat_bonds");
    CAD_CHECK(ia != nullptr && ia->elementId == 4 && ia->value == 9.0);
    // A lei 1 perde a barra 4: o número de barras registrado (2) também fica inconsistente e bloqueia
    // a barra ativa restante (elemento 1).
    CAD_CHECK(structural(ra, "barras") != nullptr && structural(ra, "barras")->lawId == 1);
    CAD_CHECK((ra.blockedElements == std::vector<std::int64_t>{1, 4}));
    show("mat_bonds = 9", ra, 2);

    dem::MatlabLawTable b = base; // ID de lei fora de ordem
    b.laws[1].id = 5;
    CAD_CHECK(structural(dem::validateLawTable(b, {}), "id") != nullptr);

    dem::MatlabLawTable c = base; // curva de descarga diferente de j + jj
    c.laws[2].unloadingCurveId = 1;
    const dem::LawValidationReport rc = dem::validateLawTable(c, {});
    CAD_CHECK(structural(rc, "curva de descarga") != nullptr && structural(rc, "curva") != nullptr);

    dem::MatlabLawTable d = base; // jj diferente de round(k, −1) + 10
    d.curveOffset = 2;
    CAD_CHECK(structural(dem::validateLawTable(d, {}), "jj") != nullptr);

    dem::MatlabLawTable e = base; // número de barras registrado
    e.laws[0].assignedBonds = 7;
    CAD_CHECK(structural(dem::validateLawTable(e, {}), "barras") != nullptr);

    dem::MatlabLawTable f = base; // valor da barra diferente do da lei
    f.bondFp[3] = 1.0;
    const dem::LawValidationReport rf = dem::validateLawTable(f, {});
    const dem::LawIssue* jf = structural(rf, "FP");
    CAD_CHECK(jf != nullptr && jf->elementId == 4 && jf->lawId == 1);

    dem::MatlabLawTable g = base; // primeira barra incoerente
    g.laws[1].firstBond = 4;
    CAD_CHECK(structural(dem::validateLawTable(g, {}), "lawBond") != nullptr);

    dem::MatlabLawTable h = base; // vetores por barra de tamanhos diferentes
    h.bondKr.pop_back();
    CAD_CHECK(structural(dem::validateLawTable(h, {}), "mat_bonds") != nullptr);

    // deleted com tamanho diferente do número de barras.
    const dem::LawValidationReport rd = dem::validateLawTable(base, {false, false});
    CAD_CHECK(structural(rd, "deleted") != nullptr && rd.issues.size() == 1);

    // Tabela inválida.
    const dem::MatlabLawTable bad = table({4, 4}, {1e-3}, {1.0, 1.0});
    const dem::LawValidationReport rb = dem::validateLawTable(bad, {});
    CAD_CHECK(!bad.ok && rb.blocksExport() && rb.issues.size() == 1
              && rb.issues[0].category == dem::LawIssueCategory::Structural);
    show("tabela invalida", rb, 1);

    // Referência inexistente numa barra excluída: o EMODIF continua escrito → InvalidEmitted.
    const dem::LawValidationReport rx = dem::validateLawTable(a, {false, false, false, true});
    CAD_CHECK(rx.blocksExport() && rx.count(dem::LawIssueKind::InvalidEmitted) == 1);
    CAD_CHECK((rx.blockedElements == std::vector<std::int64_t>{1})); // só a barra ativa da lei 1
}

// ------------------------------------------------------------------ [G]
void testLengths()
{
    std::printf("[G] comprimentos diferentes na mesma lei\n");
    // Caso "comprimentos" da E5.7.4b.6: tp = [4 4 2 2], Li = [1 1,2 1 0,8] mm, Gf = [1 1 0,7 0,7].
    const dem::MatlabLawTable t = table({4, 4, 2, 2}, {1e-3, 1.2e-3, 1e-3, 0.8e-3}, {1, 1, 0.7, 0.7});
    const dem::MatlabLawTable copy = t;
    CAD_CHECK(t.ok && t.lawCount() == 2);
    const dem::LawValidationReport r = dem::validateLawTable(t, {});
    CAD_CHECK(!r.blocksExport() && r.issues.size() == 2 && r.count(dem::LawIssueKind::Informative) == 2);
    const dem::LawIssue* a = find(r, dem::LawIssueKind::Informative, 1);
    const dem::LawIssue* b = find(r, dem::LawIssueKind::Informative, 2);
    CAD_CHECK(a != nullptr && b != nullptr);
    if (a && b)
    {
        CAD_CHECK(a->elementId == 2 && b->elementId == 4 && a->severity() == dem::LawIssueSeverity::Warning);
        CAD_CHECK_NEAR(a->value, 0.2, 1e-12);
        CAD_CHECK_NEAR(b->value, 0.2, 1e-12);
    }
    CAD_CHECK(sameTable(t, copy) && t.laws[0].li == 1e-3 && t.laws[1].li == 1e-3);
    CAD_CHECK((t.bondLaw == std::vector<std::int32_t>{1, 1, 2, 2}));
    show("comprimentos", r, 2);
}

// ------------------------------------------------------------------ [H]
void testEmptyDeleted()
{
    std::printf("[H] vetor deleted vazio = nenhuma barra excluida\n");
    const dem::MatlabLawTable tables[] = {tp3Table(), table({4, 4, 2, 2}, {1e-3, 1.2e-3, 1e-3, 0.8e-3}, {1, 1, 0.7, 0.7}),
                                          table({4, 0, 4}, {1e-3, 1e-3, 1e-3}, {0.0, -1.0, 1.0})};
    for (const dem::MatlabLawTable& t : tables)
    {
        const dem::LawValidationReport a = dem::validateLawTable(t, {});
        const dem::LawValidationReport b = dem::validateLawTable(t, std::vector<bool>(t.bondLaw.size(), false));
        bool same = a.issues.size() == b.issues.size() && a.blocksExport() == b.blocksExport()
                    && a.blockedElements == b.blockedElements && a.invalidLaws == b.invalidLaws
                    && a.deletedBonds == 0 && b.deletedBonds == 0;
        for (std::size_t i = 0; same && i < a.issues.size(); ++i)
            same = dem::describe(a.issues[i]) == dem::describe(b.issues[i]);
        CAD_CHECK(same);
    }
    // Tabela vazia: nada a validar.
    const dem::LawValidationReport e = dem::validateLawTable(table({}, {}, {}), {});
    CAD_CHECK(e.issues.empty() && !e.blocksExport());
    std::printf("  vazio e todos false produzem relatorios identicos (3 tabelas); tabela vazia sem diagnosticos\n");
}

// ------------------------------------------------------------------ [J]
// Diagnóstico com escopo, lei, elemento e parâmetro dados (nullptr se não houver).
const dem::LawIssue* findAt(const dem::LawValidationReport& r, dem::LawIssueScope scope, std::int32_t lawId,
                            std::int64_t element, const std::string& parameter)
{
    for (const dem::LawIssue& i : r.issues)
        if (i.scope == scope && i.lawId == lawId && i.elementId == element && i.parameter == parameter)
            return &i;
    return nullptr;
}

bool contains(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

// Sem mensagens contraditórias: o mesmo alvo (escopo, lei, elemento, parâmetro) nunca aparece ao mesmo
// tempo como bloqueante e como não bloqueante; todo aviso de barra excluída com kr não finito explica
// que não decide sozinho a validade da lei compartilhada nem a do bloco APDL emitido.
bool coherent(const dem::LawValidationReport& r)
{
    for (std::size_t a = 0; a < r.issues.size(); ++a)
    {
        const dem::LawIssue& x = r.issues[a];
        for (std::size_t b = a + 1; b < r.issues.size(); ++b)
        {
            const dem::LawIssue& y = r.issues[b];
            if (x.scope == y.scope && x.lawId == y.lawId && x.elementId == y.elementId && x.parameter == y.parameter
                && x.blocksExport() != y.blocksExport())
                return false;
        }
        if (x.scope == dem::LawIssueScope::Bond && x.parameter == "kr" && x.elementDeleted
            && !(x.kind == dem::LawIssueKind::Informative && !x.blocksExport()
                 && contains(x.reason, "não determina sozinho a validade da lei compartilhada")))
            return false;
    }
    return true;
}

void testNonFiniteKr()
{
    std::printf("[J] kr nao finito (Li subnormal): origem da lei e elemento afetado\n");
    const double tiny = 1e-320; // Li subnormal > 0: aceito pela tabela; kr = LCR/Li transborda para Inf
    const double lcr = lattice().lcr;

    // [D] Li subnormal aceito pela tabela, sem alteração da regra de reutilização do MATLAB.
    const dem::MatlabLawTable t = table({4, 4}, {tiny, 1e-3}, {1.0, 1.0});
    const dem::MatlabLawTable copy = t;
    CAD_CHECK(t.ok && t.lawCount() == 1 && std::fpclassify(t.laws[0].li) == FP_SUBNORMAL);
    CAD_CHECK((t.bondLaw == std::vector<std::int32_t>{1, 1}) && t.laws[0].firstBond == 1);
    CAD_CHECK(std::isinf(t.bondKr[0]) && std::isinf(t.laws[0].kr) && t.bondKr[1] == lcr / 1e-3);
    CAD_CHECK(contains(matlabLawBlock(t), "inf")); // o bloco da lei seria escrito com "inf"

    // [A] primeira barra excluída com kr = Inf, sem barras ativas na lei (a barra 2 tem outra lei).
    const dem::MatlabLawTable ta = table({4, 4}, {tiny, 1e-3}, {1.0, 1.2});
    CAD_CHECK(ta.ok && ta.lawCount() == 2 && (ta.bondLaw == std::vector<std::int32_t>{1, 2}));
    const dem::LawValidationReport ra = dem::validateLawTable(ta, {true, false});
    const dem::LawIssue* la = findAt(ra, dem::LawIssueScope::Law, 1, 1, "kr");
    const dem::LawIssue* ba = findAt(ra, dem::LawIssueScope::Bond, 1, 1, "kr");
    CAD_CHECK(la != nullptr && ba != nullptr);
    if (la)
    {
        CAD_CHECK(la->kind == dem::LawIssueKind::InvalidEmitted && la->blocksExport() && std::isinf(la->value));
        CAD_CHECK(la->originElementId == 1 && la->originDeleted && la->elementDeleted && la->activeElements == 0);
        const std::string s = dem::describe(*la);
        CAD_CHECK(contains(s, "escopo: lei e bloco APDL") && contains(s, "elemento 1 (excluído, criou a lei)")
                  && contains(s, "valor armazenado na lei kr = Inf") && contains(s, "continuam no arquivo"));
    }
    if (ba)
    {
        CAD_CHECK(ba->kind == dem::LawIssueKind::Informative && !ba->blocksExport() && ba->elementDeleted);
        const std::string s = dem::describe(*ba);
        CAD_CHECK(contains(s, "escopo: barra") && contains(s, "valor da barra kr = Inf")
                  && contains(s, "Esta barra criou a lei 1") && contains(s, "não determina sozinho"));
    }
    // [G] a lei inválida continua no APDL: InvalidEmitted bloqueia, sem elemento ativo bloqueado.
    CAD_CHECK(ra.blocksExport() && ra.count(dem::LawIssueKind::InvalidExported) == 0);
    CAD_CHECK(find(ra, dem::LawIssueKind::InvalidEmitted, 1, "d11(4)") != nullptr);
    CAD_CHECK(find(ra, dem::LawIssueKind::InvalidEmitted, 1, "d21(5)") != nullptr);
    CAD_CHECK(ra.invalidLaws == std::vector<std::int32_t>{1} && ra.blockedElements.empty());
    CAD_CHECK(find(ra, dem::LawIssueKind::InvalidExported, 2) == nullptr && find(ra, dem::LawIssueKind::InvalidEmitted, 2) == nullptr);
    CAD_CHECK(coherent(ra));
    show("[A] lei 1 so na barra 1 excluida (kr = Inf)", ra, 3);

    // [B] e [E] primeira barra excluída com kr = Inf; barra 2 ativa reutiliza a lei.
    const dem::LawValidationReport rb = dem::validateLawTable(t, {true, false});
    const dem::LawIssue* lb = findAt(rb, dem::LawIssueScope::Law, 1, 2, "kr");
    CAD_CHECK(lb != nullptr);
    if (lb)
    {
        CAD_CHECK(lb->kind == dem::LawIssueKind::InvalidExported && lb->blocksExport() && std::isinf(lb->value));
        CAD_CHECK(lb->elementId == 2 && !lb->elementDeleted && lb->originElementId == 1 && lb->originDeleted);
        CAD_CHECK(lb->tp == 4 && lb->activeElements == 1 && lb->category == dem::LawIssueCategory::Numeric);
        const std::string s = dem::describe(*lb);
        CAD_CHECK(contains(s, "elemento 2 (ativo), afetado pela lei criada pelo elemento 1 (excluído)"));
        CAD_CHECK(contains(s, "valor armazenado na lei kr = Inf"));
        CAD_CHECK(contains(s, "proveniente do elemento 1 (excluído), que criou a lei"));
        // O kr geométrico do elemento 2 é finito e citado como tal, nunca como Inf.
        CAD_CHECK(contains(s, ("o elemento afetado 2 reutiliza a lei e seu kr geométrico ("
                               + dem::formatIssueValue(t.bondKr[1]) + ") não é usado").c_str()));
    }
    // Nenhum diagnóstico atribui kr = Inf à barra 2: no escopo barra não há kr para o elemento 2.
    CAD_CHECK(findAt(rb, dem::LawIssueScope::Bond, 1, 2, "kr") == nullptr);
    for (const dem::LawIssue& i : rb.issues)
        CAD_CHECK(!(i.scope == dem::LawIssueScope::Bond && i.elementId == 2));
    const dem::LawIssue* bb = findAt(rb, dem::LawIssueScope::Bond, 1, 1, "kr");
    CAD_CHECK(bb != nullptr && bb->kind == dem::LawIssueKind::Informative && !bb->blocksExport());
    CAD_CHECK(rb.blocksExport() && rb.invalidLaws == std::vector<std::int32_t>{1});
    CAD_CHECK((rb.blockedElements == std::vector<std::int64_t>{2}));
    CAD_CHECK(find(rb, dem::LawIssueKind::InvalidExported, 1, "d11(4)") != nullptr);
    CAD_CHECK(coherent(rb)); // [F]
    show("[B] barra 1 excluida cria a lei (kr = Inf), barra 2 ativa reutiliza", rb, 3);

    // [C] barra ativa posterior com kr = Inf; a lei usa os valores finitos da primeira barra.
    const dem::MatlabLawTable tc = table({4, 4}, {1e-3, tiny}, {1.0, 1.0});
    CAD_CHECK(tc.ok && tc.lawCount() == 1 && tc.laws[0].firstBond == 1 && std::isinf(tc.bondKr[1]));
    CAD_CHECK(std::isfinite(tc.laws[0].kr) && tc.laws[0].kr == tc.bondKr[0]);
    // O texto APDL das leis é o mesmo de uma tabela só com a primeira barra (kr da barra 2 não é escrito).
    CAD_CHECK(matlabLawBlock(tc) == matlabLawBlock(table({4}, {1e-3}, {1.0})) && !contains(matlabLawBlock(tc), "inf"));
    const dem::LawValidationReport rc = dem::validateLawTable(tc, {});
    CAD_CHECK(findAt(rc, dem::LawIssueScope::Law, 1, 1, "kr") == nullptr); // a lei não tem kr inválido
    const dem::LawIssue* bc = findAt(rc, dem::LawIssueScope::Bond, 1, 2, "kr");
    CAD_CHECK(bc != nullptr);
    if (bc)
    {
        // Política conservadora preservada: barra ativa com kr não finito bloqueia.
        CAD_CHECK(bc->kind == dem::LawIssueKind::InvalidExported && bc->blocksExport() && std::isinf(bc->value));
        CAD_CHECK(bc->originElementId == 1 && !bc->originDeleted && !bc->elementDeleted);
        const std::string s = dem::describe(*bc);
        CAD_CHECK(contains(s, "escopo: barra") && contains(s, "elemento 2 (ativo), afetado pela lei criada pelo elemento 1 (ativo)"));
        CAD_CHECK(contains(s, "valor da barra kr = Inf") && contains(s, "não é usado na curva nem escrito no APDL")
                  && contains(s, "Bloqueio conservador"));
        CAD_CHECK(contains(s, ("kr armazenado na lei = " + dem::formatIssueValue(tc.laws[0].kr)).c_str()));
    }
    CAD_CHECK(rc.blocksExport() && rc.invalidLaws == std::vector<std::int32_t>{1});
    CAD_CHECK((rc.blockedElements == std::vector<std::int64_t>{1, 2}));
    CAD_CHECK(coherent(rc));
    show("[C] barra 2 ativa com kr = Inf, lei finita", rc, 1);
    // Mesma barra posterior excluída: só aviso, sem bloqueio (a lei e o bloco emitido são válidos).
    const dem::LawValidationReport rc2 = dem::validateLawTable(tc, {false, true});
    const dem::LawIssue* bc2 = findAt(rc2, dem::LawIssueScope::Bond, 1, 2, "kr");
    CAD_CHECK(bc2 != nullptr && bc2->kind == dem::LawIssueKind::Informative && !rc2.blocksExport());
    if (bc2)
        CAD_CHECK(contains(dem::describe(*bc2), "não determina sozinho a validade da lei compartilhada"));
    CAD_CHECK(rc2.invalidLaws.empty() && rc2.blockedElements.empty() && coherent(rc2));

    // [D] tp = 3 com Li subnormal na primeira barra, todas excluídas: ReferenceZero e InvalidEmitted
    // (abscissas NaN = 0·Inf) coexistem sem contradição.
    const dem::MatlabLawTable t3 = table({3, 3}, {tiny, 1e-3}, {1.0, 1.0});
    const dem::LawValidationReport r3 = dem::validateLawTable(t3, {true, true});
    CAD_CHECK(find(r3, dem::LawIssueKind::ReferenceZero, 1, "FP") != nullptr);
    CAD_CHECK(find(r3, dem::LawIssueKind::InvalidEmitted, 1, "d11(4)") != nullptr && r3.blocksExport());
    CAD_CHECK(r3.blockedElements.empty() && coherent(r3));
    CAD_CHECK(sameTable(t, copy)); // a validação não altera a tabela
}

} // namespace

int main()
{
    testDemDyna(CAD_MATLAB_REFERENCE_DIR "/DEM_DYNA.txt");
    testTp3Active();
    testTp3OnlyDeleted();
    testKr();
    testValues();
    testStructural();
    testLengths();
    testEmptyDeleted();
    testNonFiniteKr();
    return cad::test::finish("test_e574b7");
}
