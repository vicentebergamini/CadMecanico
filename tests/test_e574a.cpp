// E5.7.4a sem interface — propriedades físicas do modelo MED:
//  - criação das estruturas (SI) e valores padrão de entrada de DEM_HIAGO_PRINCIPAL.m;
//  - validação (regras do MATLAB e da formulação: ν em (−1; 0,5), ρ, E, GRF e RFC positivos) e
//    rejeição de NaN, infinito e texto não numérico;
//  - CORRCAF = 1 constante (estrutura, documento e arquivo);
//  - conversões mm → m de comprimentos, áreas e volumes;
//  - armazenamento e recuperação no .cadmec (esquema 10), independência entre projetos e abertura
//    de projetos antigos (esquemas 1 a 9);
//  - discretização e exportação APDL idênticas com e sem propriedades MED no projeto.
#include "TestCheck.h"

#include "dem/DemProperties.h"
#include "dem/DemUnits.h"
#include "discretization/ApdlExporter.h"
#include "discretization/VoxelDiscretizer.h"
#include "document/CadDocument.h"
#include "document/DocumentFile.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>

using namespace cad::dem;
using namespace cad::document;
namespace fs = std::filesystem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

DemProperties sample()
{
    DemProperties p;
    p.material.density = 11.9;
    p.material.poissonRatio = 0.25;
    p.material.youngModulus = 4.35e6;
    p.material.fractureEnergy = 133.4;
    p.model.failureFactor = 3.89;
    return p;
}

QJsonObject rootOf(const QByteArray& json)
{
    return QJsonDocument::fromJson(json).object();
}

QByteArray withDem(const QByteArray& json, const std::function<void(QJsonObject&)>& edit)
{
    QJsonObject root = rootOf(json);
    QJsonObject doc = root["document"].toObject();
    QJsonObject dem = doc["demProperties"].toObject();
    edit(dem);
    doc["demProperties"] = dem;
    root["document"] = doc;
    return QJsonDocument(root).toJson();
}

std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// 1. Criação das estruturas.
void creation()
{
    const DemProperties empty;
    CAD_CHECK(empty.material.density == 0.0 && empty.material.poissonRatio == 0.0);
    CAD_CHECK(empty.material.youngModulus == 0.0 && empty.material.fractureEnergy == 0.0);
    CAD_CHECK(empty.model.failureFactor == 0.0);
    CAD_CHECK(!validate(empty).ok); // estrutura vazia não é um conjunto válido de propriedades

    const DemProperties d = matlabDefaults(); // lerValor(..., padrão) de DEM_HIAGO_PRINCIPAL.m
    CAD_CHECK(d.material.density == 1240.0);
    CAD_CHECK(d.material.poissonRatio == 0.36);
    CAD_CHECK(d.material.youngModulus == 3.5e9);
    CAD_CHECK(d.material.fractureEnergy == 2400.0);
    CAD_CHECK(d.model.failureFactor == 10.0);
    CAD_CHECK(d.model.corrcaf() == 1.0);
    CAD_CHECK(validate(d).ok);
    CAD_CHECK(d == matlabDefaults() && d != sample());
}

// 2 e 3. Validação e rejeição de valores inválidos.
void validation()
{
    CAD_CHECK(validate(sample()).ok);
    auto rejects = [](const std::function<void(DemProperties&)>& edit, const char* field) {
        DemProperties p = sample();
        edit(p);
        const PropertyValidation v = validate(p);
        if (v.ok || v.field != field)
            std::printf("  esperado erro em %s, obtido ok=%d campo=%s\n", field, int(v.ok), v.field.c_str());
        return !v.ok && v.field == field && !v.message.empty();
    };
    auto accepts = [](const std::function<void(DemProperties&)>& edit) {
        DemProperties p = sample();
        edit(p);
        return validate(p).ok;
    };
    // NaN e infinito em todos os campos.
    for (double bad : {kNaN, kInf, -kInf})
    {
        CAD_CHECK(rejects([bad](DemProperties& p) { p.material.density = bad; }, kFieldDensity));
        CAD_CHECK(rejects([bad](DemProperties& p) { p.material.poissonRatio = bad; }, kFieldPoissonRatio));
        CAD_CHECK(rejects([bad](DemProperties& p) { p.material.youngModulus = bad; }, kFieldYoungModulus));
        CAD_CHECK(rejects([bad](DemProperties& p) { p.material.fractureEnergy = bad; }, kFieldFractureEnergy));
        CAD_CHECK(rejects([bad](DemProperties& p) { p.model.failureFactor = bad; }, kFieldFailureFactor));
    }
    // Densidade e módulo de elasticidade positivos.
    CAD_CHECK(rejects([](DemProperties& p) { p.material.density = 0.0; }, kFieldDensity));
    CAD_CHECK(rejects([](DemProperties& p) { p.material.density = -1240.0; }, kFieldDensity));
    CAD_CHECK(accepts([](DemProperties& p) { p.material.density = 1e-9; }));
    CAD_CHECK(rejects([](DemProperties& p) { p.material.youngModulus = 0.0; }, kFieldYoungModulus));
    CAD_CHECK(rejects([](DemProperties& p) { p.material.youngModulus = -3.5e9; }, kFieldYoungModulus));
    // Poisson: intervalo aberto (−1; 0,5) de DEM_HIAGO_PRINCIPAL.m (sem outros limites).
    CAD_CHECK(rejects([](DemProperties& p) { p.material.poissonRatio = -1.0; }, kFieldPoissonRatio));
    CAD_CHECK(rejects([](DemProperties& p) { p.material.poissonRatio = 0.5; }, kFieldPoissonRatio));
    CAD_CHECK(rejects([](DemProperties& p) { p.material.poissonRatio = 0.7; }, kFieldPoissonRatio));
    CAD_CHECK(accepts([](DemProperties& p) { p.material.poissonRatio = -0.999; }));
    CAD_CHECK(accepts([](DemProperties& p) { p.material.poissonRatio = 0.0; }));
    CAD_CHECK(accepts([](DemProperties& p) { p.material.poissonRatio = 0.4999; }));
    // GRF e RFC positivos (formulação: √(GRF/...) e 1/RFC²); nenhum limite superior.
    CAD_CHECK(rejects([](DemProperties& p) { p.material.fractureEnergy = 0.0; }, kFieldFractureEnergy));
    CAD_CHECK(rejects([](DemProperties& p) { p.material.fractureEnergy = -2400.0; }, kFieldFractureEnergy));
    CAD_CHECK(accepts([](DemProperties& p) { p.material.fractureEnergy = 1e12; }));
    CAD_CHECK(rejects([](DemProperties& p) { p.model.failureFactor = 0.0; }, kFieldFailureFactor));
    CAD_CHECK(rejects([](DemProperties& p) { p.model.failureFactor = -10.0; }, kFieldFailureFactor));
    CAD_CHECK(accepts([](DemProperties& p) { p.model.failureFactor = 1e-6; }));
    CAD_CHECK(accepts([](DemProperties& p) { p.model.failureFactor = 1e6; }));

    // Texto → número.
    double v = 0.0;
    CAD_CHECK(parseNumber("3.5e9", v) && v == 3.5e9);
    CAD_CHECK(parseNumber("3,5e9", v) && v == 3.5e9);
    CAD_CHECK(parseNumber("0,36", v) && v == 0.36);
    CAD_CHECK(parseNumber("  1240  ", v) && v == 1240.0);
    CAD_CHECK(parseNumber("-0.5", v) && v == -0.5);
    CAD_CHECK(parseNumber("+2E-3", v) && v == 2e-3);
    v = 7.0;
    for (const char* bad : {"", "   ", "abc", "1.2.3", "1,240.5", "1.240,5", "1,2,3", "nan", "NaN", "inf", "-inf",
                            "Infinity", "1e999", "0x10", "12abc", "1 2", "e5", ".", "-", "1e"})
    {
        const bool ok = parseNumber(bad, v);
        if (ok)
            std::printf("  texto aceito indevidamente: \"%s\"\n", bad);
        CAD_CHECK(!ok);
    }
    CAD_CHECK(v == 7.0); // valor de saída inalterado nas recusas
}

// 4. CORRCAF = 1.
void corrcaf()
{
    static_assert(ModelParameters::kCorrcaf == 1.0, "CORRCAF deve ser 1");
    static_assert(ModelParameters::corrcaf() == 1.0, "CORRCAF deve ser 1");
    // Não há campo CORRCAF: a estrutura só guarda RFC.
    static_assert(sizeof(ModelParameters) == sizeof(double), "ModelParameters guarda apenas RFC");

    CadDocument doc("CORRCAF");
    CAD_CHECK(doc.setDemProperties(sample()).ok);
    const QByteArray json = DocumentFile::toJson(doc);
    const QJsonObject model = rootOf(json)["document"].toObject()["demProperties"].toObject()["model"].toObject();
    CAD_CHECK(model["corrcaf"].toDouble() == 1.0);

    // Arquivo com CORRCAF ≠ 1 (ou ausente) é recusado e o documento ativo não muda.
    for (const QJsonValue& value : {QJsonValue(2.0), QJsonValue(0.0), QJsonValue(1.0000001), QJsonValue("1")})
    {
        CadDocument out("ativo");
        const FileResult r = DocumentFile::fromJson(
            withDem(json, [&](QJsonObject& dem) {
                QJsonObject m = dem["model"].toObject();
                m["corrcaf"] = value;
                dem["model"] = m;
            }),
            out);
        CAD_CHECK(!r.ok);
        CAD_CHECK(out.name() == "ativo" && !out.demProperties());
    }
    CadDocument out("ativo");
    CAD_CHECK(!DocumentFile::fromJson(withDem(json, [](QJsonObject& dem) {
                                         QJsonObject m = dem["model"].toObject();
                                         m.remove("corrcaf");
                                         dem["model"] = m;
                                     }),
                                     out)
                   .ok);
    CadDocument loaded;
    CAD_CHECK(DocumentFile::fromJson(json, loaded).ok);
    CAD_CHECK(loaded.demProperties() && loaded.demProperties()->model.corrcaf() == 1.0);
}

// 5 e 6. Conversões de unidades (geometria CAD em mm, modelo físico em m).
void unitConversions()
{
    using namespace cad::dem::units;
    CAD_CHECK(std::string(kCadLengthUnit) == "mm" && std::string(kDemLengthUnit) == "m");
    CAD_CHECK(std::string(kUnitSystem) == "SI");
    static_assert(mmToM(1000.0) == 1.0, "1000 mm = 1 m");
    CAD_CHECK(mmToM(1.0) == 0.001);
    CAD_CHECK(mmToM(10.0) == 0.01);
    CAD_CHECK(mmToM(0.0) == 0.0);
    CAD_CHECK(mmToM(-25.0) == -0.025);
    CAD_CHECK(mToMm(0.001) == 1.0);
    CAD_CHECK(mToMm(2.5) == 2500.0);
    CAD_CHECK(mm2ToM2(1.0e6) == 1.0);
    CAD_CHECK(mm2ToM2(100.0) == 1.0e-4);
    CAD_CHECK(m2ToMm2(1.0e-4) == 100.0);
    CAD_CHECK(mm3ToM3(1.0e9) == 1.0);
    CAD_CHECK(mm3ToM3(1000.0) == 1.0e-6); // cubo de aresta LCO = 10 mm
    CAD_CHECK(m3ToMm3(1.0e-6) == 1000.0);
    // Ida e volta e coerência entre comprimento, área e volume (erro de arredondamento apenas).
    for (double x : {0.1, 0.3, 1.0, 2.5, 10.0, 33.3, 123.456, 9999.0})
    {
        CAD_CHECK(std::fabs(mToMm(mmToM(x)) - x) <= 1e-15 * x);
        CAD_CHECK(std::fabs(m2ToMm2(mm2ToM2(x)) - x) <= 1e-15 * x);
        CAD_CHECK(std::fabs(m3ToMm3(mm3ToM3(x)) - x) <= 1e-15 * x);
        CAD_CHECK(std::fabs(mm2ToM2(x * x) - mmToM(x) * mmToM(x)) <= 1e-15 * mm2ToM2(x * x));
        CAD_CHECK(std::fabs(mm3ToM3(x * x * x) - std::pow(mmToM(x), 3)) <= 1e-15 * mm3ToM3(x * x * x));
    }
    // Ponto: cópia convertida; o original (CAD, mm) não é alterado.
    const double cad[3] = {10.0, -2.5, 1240.0};
    const PointM p = pointMmToM(cad[0], cad[1], cad[2]);
    CAD_CHECK(p.x == 0.01 && p.y == -0.0025 && p.z == 1.24);
    CAD_CHECK(cad[0] == 10.0 && cad[1] == -2.5 && cad[2] == 1240.0);
}

// 7, 8 e 9. Armazenamento, recuperação e independência entre projetos.
void persistence(const fs::path& dir)
{
    CadDocument a = CadDocument::withTestPart("Projeto A");
    CadDocument b = CadDocument::withTestPart("Projeto B");
    CAD_CHECK(!a.demProperties() && !b.demProperties());
    CAD_CHECK(!a.isModified());

    // Valores inválidos: nada muda.
    DemProperties bad = sample();
    bad.material.poissonRatio = 0.5;
    CAD_CHECK(!a.setDemProperties(bad).ok);
    CAD_CHECK(!a.demProperties() && !a.isModified());

    const int revision = a.revision();
    const TopoDS_Shape shapeBefore = a.shape();
    CAD_CHECK(a.setDemProperties(sample()).ok);
    CAD_CHECK(a.demProperties() && *a.demProperties() == sample());
    CAD_CHECK(a.isModified());
    CAD_CHECK(a.revision() == revision && a.shape().IsSame(shapeBefore)); // geometria não recalculada
    CAD_CHECK(!b.demProperties());                                         // projeto B não muda

    // Valores iguais não marcam o documento como modificado.
    a.setModified(false);
    CAD_CHECK(a.setDemProperties(sample()).ok && !a.isModified());

    CAD_CHECK(b.setDemProperties(matlabDefaults()).ok);
    const fs::path fa = dir / "projeto_a.cadmec";
    const fs::path fb = dir / "projeto_b.cadmec";
    CAD_CHECK(DocumentFile::save(a, QString::fromStdWString(fa.wstring())).ok);
    CAD_CHECK(DocumentFile::save(b, QString::fromStdWString(fb.wstring())).ok);

    const QJsonObject root = rootOf(QByteArray::fromStdString(readText(fa)));
    CAD_CHECK(root["schemaVersion"].toInt() == 10);
    const QJsonObject doc = root["document"].toObject();
    CAD_CHECK(doc["unit"].toString() == "mm"); // geometria continua em mm
    const QJsonObject dem = doc["demProperties"].toObject();
    CAD_CHECK(dem["unitSystem"].toString() == "SI");
    CAD_CHECK(dem["material"].toObject()["density"].toDouble() == 11.9);
    CAD_CHECK(dem["material"].toObject()["youngModulus"].toDouble() == 4.35e6);
    CAD_CHECK(dem["model"].toObject()["failureFactor"].toDouble() == 3.89);

    CadDocument la, lb;
    CAD_CHECK(DocumentFile::load(QString::fromStdWString(fa.wstring()), la).ok);
    CAD_CHECK(DocumentFile::load(QString::fromStdWString(fb.wstring()), lb).ok);
    CAD_CHECK(la.demProperties() && *la.demProperties() == sample());
    CAD_CHECK(lb.demProperties() && *lb.demProperties() == matlabDefaults());
    CAD_CHECK(!la.isModified() && !lb.isModified());
    CAD_CHECK(la.parameters() == a.parameters()); // geometria preservada

    // Alterar e regravar A não altera o arquivo de B.
    const std::string bBefore = readText(fb);
    DemProperties changed = sample();
    changed.material.density = 2000.0;
    CAD_CHECK(la.setDemProperties(changed).ok);
    CAD_CHECK(DocumentFile::save(la, QString::fromStdWString(fa.wstring())).ok);
    CAD_CHECK(readText(fb) == bBefore);
    CadDocument la2;
    CAD_CHECK(DocumentFile::load(QString::fromStdWString(fa.wstring()), la2).ok);
    CAD_CHECK(la2.demProperties()->material.density == 2000.0);
    CAD_CHECK(*lb.demProperties() == matlabDefaults());

    // Documento sem propriedades: o campo não é gravado e a leitura mantém "não definidas".
    const QByteArray noDem = DocumentFile::toJson(CadDocument::withTestPart("Sem MED"));
    CAD_CHECK(!rootOf(noDem)["document"].toObject().contains("demProperties"));
    CadDocument n;
    CAD_CHECK(DocumentFile::fromJson(noDem, n).ok && !n.demProperties());
}

// 10. Projetos antigos e arquivos com propriedades inválidas.
void compatibility()
{
    // Arquivos de referência dos esquemas 1 a 6 abrem sem propriedades MED.
    for (const char* file : {"documento_e51_v1.cadmec", "documento_e52_v2.cadmec", "documento_e521_v3.cadmec",
                             "documento_e53_v4.cadmec", "documento_e54_v5.cadmec", "documento_e55_v6.cadmec"})
    {
        CadDocument doc;
        const FileResult r = DocumentFile::load(QStringLiteral(CADMEC_TEST_DATA_DIR "/") + file, doc);
        if (!r.ok)
            std::printf("  %s: %s\n", file, qPrintable(r.message));
        CAD_CHECK(r.ok);
        CAD_CHECK(!doc.demProperties());
        // Regravado na versão atual sem criar propriedades.
        const QJsonObject root = rootOf(DocumentFile::toJson(doc));
        CAD_CHECK(root["schemaVersion"].toInt() == DocumentFile::kSchemaVersion);
        CAD_CHECK(!root["document"].toObject().contains("demProperties"));
    }
    // Esquemas 7 a 9 (gerados a partir do atual): abrem sem propriedades; com "demProperties" são recusados.
    CadDocument with = CadDocument::withTestPart("Com MED");
    CAD_CHECK(with.setDemProperties(sample()).ok);
    const QByteArray json = DocumentFile::toJson(with);
    for (int schema : {7, 8, 9})
    {
        QJsonObject root = rootOf(json);
        root["schemaVersion"] = schema;
        CadDocument out;
        CAD_CHECK(!DocumentFile::fromJson(QJsonDocument(root).toJson(), out).ok);
        QJsonObject doc = root["document"].toObject();
        doc.remove("demProperties");
        root["document"] = doc;
        CAD_CHECK(DocumentFile::fromJson(QJsonDocument(root).toJson(), out).ok && !out.demProperties());
    }

    // Estrutura, tipos e valores inválidos: arquivo recusado, documento ativo intacto.
    using Edit = std::function<void(QJsonObject&)>;
    auto setIn = [](const char* group, const char* key, const QJsonValue& value) -> Edit {
        return [=](QJsonObject& dem) {
            QJsonObject g = dem[group].toObject();
            g[key] = value;
            dem[group] = g;
        };
    };
    auto removeIn = [](const char* group, const char* key) -> Edit {
        return [=](QJsonObject& dem) {
            QJsonObject g = dem[group].toObject();
            g.remove(key);
            dem[group] = g;
        };
    };
    const Edit invalid[] = {
        setIn("material", "density", -1.0),
        setIn("material", "density", 0.0),
        setIn("material", "density", "1240"),
        setIn("material", "poissonRatio", 0.5),
        setIn("material", "poissonRatio", -1.0),
        setIn("material", "youngModulus", 0.0),
        setIn("material", "fractureEnergy", 0.0),
        setIn("material", "fractureEnergy", QJsonValue()),
        setIn("model", "failureFactor", 0.0),
        setIn("model", "failureFactor", "nan"),
        setIn("material", "extra", 1.0),
        removeIn("material", "fractureEnergy"),
        removeIn("model", "failureFactor"),
        [](QJsonObject& dem) { dem["unitSystem"] = "mm"; },
        [](QJsonObject& dem) { dem.remove("unitSystem"); },
        [](QJsonObject& dem) { dem.remove("model"); },
        [](QJsonObject& dem) { dem["extra"] = 1; },
    };
    int index = 0;
    for (const Edit& edit : invalid)
    {
        CadDocument out("ativo");
        const FileResult r = DocumentFile::fromJson(withDem(json, edit), out);
        if (r.ok)
            std::printf("  caso inválido %d aceito\n", index);
        CAD_CHECK(!r.ok);
        CAD_CHECK(out.name() == "ativo" && !out.demProperties() && out.operations().empty());
        ++index;
    }
    {
        QJsonObject root = rootOf(json);
        QJsonObject doc = root["document"].toObject();
        doc["demProperties"] = QJsonValue("SI");
        root["document"] = doc;
        CadDocument out;
        CAD_CHECK(!DocumentFile::fromJson(QJsonDocument(root).toJson(), out).ok);
    }
}

// 12 e 13. Discretização e exportação APDL inalteradas com propriedades MED no projeto.
void discretizationPreserved(const fs::path& dir)
{
    using namespace cad::discretization;
    CadDocument plain = CadDocument::withTestPart("Peça");
    CadDocument withDem = CadDocument::withTestPart("Peça");
    CAD_CHECK(withDem.setDemProperties(matlabDefaults()).ok);
    // Também após gravar e reabrir.
    CadDocument reopened;
    CAD_CHECK(DocumentFile::fromJson(DocumentFile::toJson(withDem), reopened).ok);

    Parameters params;
    params.lco = 5.0;
    params.criterion = InclusionCriterion::FullyInside;
    std::string texts[3];
    std::int64_t nodes[3] = {0, 0, 0};
    const CadDocument* docs[3] = {&plain, &withDem, &reopened};
    for (int i = 0; i < 3; ++i)
    {
        const DiscretizeResult r = VoxelDiscretizer::run(docs[i]->shape(), params);
        CAD_CHECK(r.ok && r.data);
        if (!r.ok || !r.data)
            return;
        ApdlOptions options;
        options.timestamp = "27/10/2026, 12:00:00";
        options.partName = "Peça";
        const fs::path file = dir / ("apdl_" + std::to_string(i) + ".inp");
        const ApdlExportResult e = ApdlExporter().write(*r.data, file, options, true);
        CAD_CHECK(e.ok);
        CAD_CHECK(ApdlExporter::verify(file, *r.data, options).ok);
        nodes[i] = e.nodes;
        texts[i] = readText(file);
    }
    CAD_CHECK(nodes[0] > 0 && nodes[0] == nodes[1] && nodes[0] == nodes[2]);
    CAD_CHECK(!texts[0].empty() && texts[0] == texts[1] && texts[0] == texts[2]);
    // A exportação geométrica não passou a conter propriedades físicas.
    for (const char* command : {"TB,", "TBDAT", "EDCURVE", "MP,", "R,"})
        CAD_CHECK(texts[1].find(std::string("\n") + command) == std::string::npos);
}

} // namespace

int main(int argc, char** argv)
{
    const fs::path dir = fs::path(argc > 1 ? argv[1] : "saida_e574a");
    fs::create_directories(dir);

    creation();
    validation();
    corrcaf();
    unitConversions();
    persistence(dir);
    compatibility();
    discretizationPreserved(dir);
    return cad::test::finish("e574a");
}
