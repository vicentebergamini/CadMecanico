// E5.7 sem interface: P4 (extrusão unificada — novo corpo, adicionar material com e sem união,
// remover material, "cortar o lado de fora", profundidade, sentido, persistência e exportação) e
// P2 (diagnóstico — operação válida/inválida, erro de reconstrução, abertura de documento com
// erro, operações bloqueadas e independentes, correção de parâmetros, recuperação e persistência).
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "document/Diagnostics.h"
#include "document/DocumentFile.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "io/StlExporter.h"
#include "modeling/ShapeInspector.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace cad::document;
using namespace cad::sketch;
using cad::modeling::ShapeInspector;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBox = 120000.0;

std::string sketchWith(CadDocument& doc, PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s = doc.createSketch(plane);
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s.addEntity(g).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

std::string extrude(CadDocument& doc, const ExtrudeParameters& p)
{
    std::string id;
    const auto r = doc.addExtrusion(p, &id);
    if (!r.ok)
        std::printf("  extrusao recusada: %s\n", r.message.c_str());
    CAD_CHECK(r.ok);
    return id;
}

ExtrudeParameters params(const std::string& sketch, double depth, ExtrudeMode mode = ExtrudeMode::NewBody,
                         const std::string& target = {}, EndCondition end = EndCondition::Blind, bool outside = false,
                         bool reversed = false)
{
    ExtrudeParameters p;
    p.sketchId = sketch;
    p.depth = depth;
    p.mode = mode;
    p.targetId = target;
    p.endCondition = end;
    p.cutOutside = outside;
    p.reversed = reversed;
    return p;
}

// Bloco 100 x 60 x 20 a partir de z = 0 e esboço XY deslocado em z = 20 com um círculo R 10
// centrado (perfil de corte sobre a face superior, em plano global XY no nível 0: o corte usa o
// sentido oposto à normal, então o esboço fica no topo via face).
TopoDS_Face topFace(const TopoDS_Shape& body)
{
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        const auto d = cad::modeling::describeFace(f);
        if (d.normal.Z() > 0.999 && std::fabs(d.offset - 20.0) < 1e-6)
            return f;
    }
    CAD_CHECK(false);
    return TopoDS_Face();
}

std::string faceSketch(CadDocument& doc, const std::string& body, const TopoDS_Face& face,
                       std::initializer_list<SketchGeometry> geometries)
{
    std::string error;
    auto s = doc.createFaceSketch(body, face, error);
    CAD_CHECK(s.has_value());
    if (!s)
        return {};
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s->addEntity(g).ok);
    CAD_CHECK(doc.commitSketch(*s, error));
    return s->id();
}

TopoDS_Edge edgeAt(const TopoDS_Shape& shape, double x, double y, double z)
{
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(x, y, z)) < 1e-6)
            return TopoDS::Edge(ex.Current());
    }
    CAD_CHECK(false);
    return TopoDS_Edge();
}

bool inside(const TopoDS_Shape& s, double x, double y, double z)
{
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-7);
    return c.State() == TopAbs_IN;
}

int count(const std::vector<Diagnostic>& list, DiagnosticSeverity severity)
{
    int n = 0;
    for (const Diagnostic& d : list)
        n += d.severity == severity ? 1 : 0;
    return n;
}

const Diagnostic* findDiagnostic(const std::vector<Diagnostic>& list, const std::string& id)
{
    for (const Diagnostic& d : list)
        if (d.itemId == id)
            return &d;
    return nullptr;
}

// Documento: bloco (op-1), esboço com círculo sobre a face superior (sk-2).
struct Base
{
    CadDocument doc;
    std::string block;
    std::string circle;
};

Base base()
{
    Base b;
    b.block = extrude(b.doc, params(sketchWith(b.doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20.0));
    b.circle = faceSketch(b.doc, b.block, topFace(b.doc.findOperation(b.block)->result), {SketchCircle{50, 30, 10}});
    return b;
}

// Substitui um parâmetro numérico de uma operação no JSON salvo.
QByteArray withParameter(const QByteArray& json, const QString& opId, const QString& key, double value)
{
    QJsonObject root = QJsonDocument::fromJson(json).object();
    QJsonObject doc = root.value("document").toObject();
    QJsonArray ops = doc.value("operations").toArray();
    for (int i = 0; i < ops.size(); ++i)
    {
        QJsonObject op = ops[i].toObject();
        if (op.value("id").toString() != opId)
            continue;
        QJsonObject p = op.value("parameters").toObject();
        p.insert(key, value);
        op.insert("parameters", p);
        ops[i] = op;
    }
    doc.insert("operations", ops);
    root.insert("document", doc);
    return QJsonDocument(root).toJson();
}

// --- P4 --------------------------------------------------------------------------

void testCorteExterno()
{
    std::printf("P4: cortar o lado de fora\n");
    // Profundidade definida (5 mm): sobra o bloco abaixo de z = 15 e o cilindro R 10 no topo.
    {
        Base b = base();
        const auto preview = b.doc.previewExtrusion(params(b.circle, 5.0, ExtrudeMode::Cut, b.block, EndCondition::Blind, true));
        CAD_CHECK(preview.ok);
        const std::string id = extrude(b.doc, params(b.circle, 5.0, ExtrudeMode::Cut, b.block, EndCondition::Blind, true));
        const double expected = 100.0 * 60.0 * 15.0 + kPi * 100.0 * 5.0;
        std::printf("  externo 5 mm: %.4f (esperado %.4f)\n", ShapeInspector::volume(b.doc.shape()), expected);
        CAD_CHECK_NEAR(ShapeInspector::volume(b.doc.shape()), expected, 1e-3);
        CAD_CHECK(inside(b.doc.shape(), 50, 30, 18));  // dentro do perfil: preservado
        CAD_CHECK(!inside(b.doc.shape(), 10, 10, 18)); // fora do perfil, na extensão: removido
        CAD_CHECK(inside(b.doc.shape(), 10, 10, 10));  // além da extensão: preservado
        CAD_CHECK(b.doc.findOperation(id)->cutOutside);
        // Ferramenta da pré-visualização = região externa (laje − prisma) na extensão.
        const auto tool = b.doc.extrusionTool(params(b.circle, 5.0, ExtrudeMode::Cut, b.block, EndCondition::Blind, true));
        CAD_CHECK(tool.ok);
        CAD_CHECK(!inside(tool.shape, 50, 30, 18) && inside(tool.shape, 10, 10, 18));
        // Corte interno com os mesmos parâmetros, para comparação: furo cego de 5 mm.
        CAD_CHECK(b.doc.updateExtrusion(id, params(b.circle, 5.0, ExtrudeMode::Cut, b.block)).ok);
        CAD_CHECK_NEAR(ShapeInspector::volume(b.doc.shape()), kBox - kPi * 100.0 * 5.0, 1e-3);
        // Alteração de profundidade e retorno ao externo pela edição.
        CAD_CHECK(b.doc.updateExtrusion(id, params(b.circle, 8.0, ExtrudeMode::Cut, b.block, EndCondition::Blind, true)).ok);
        CAD_CHECK_NEAR(ShapeInspector::volume(b.doc.shape()), 100.0 * 60.0 * 12.0 + kPi * 100.0 * 8.0, 1e-3);
    }
    // Atravessar tudo: sobra só o cilindro.
    {
        Base b = base();
        extrude(b.doc, params(b.circle, 10.0, ExtrudeMode::Cut, b.block, EndCondition::ThroughAll, true));
        std::printf("  externo atravessar tudo: %.4f (esperado %.4f)\n", ShapeInspector::volume(b.doc.shape()),
                    kPi * 100.0 * 20.0);
        CAD_CHECK_NEAR(ShapeInspector::volume(b.doc.shape()), kPi * 100.0 * 20.0, 1e-3);
        CAD_CHECK(b.doc.solidCount() == 1);
    }
    // Sentido invertido (para fora da face): não há material na extensão → recusado, sem mudança.
    {
        Base b = base();
        const int revision = b.doc.revision();
        const auto r = b.doc.addExtrusion(params(b.circle, 5.0, ExtrudeMode::Cut, b.block, EndCondition::Blind, true, true));
        std::printf("  externo invertido: %s\n", r.message.c_str());
        CAD_CHECK(!r.ok && r.message.find("fora do perfil") != std::string::npos);
        CAD_CHECK(b.doc.revision() == revision);
    }
    // Só no corte.
    {
        Base b = base();
        const auto r = b.doc.addExtrusion(params(b.circle, 5.0, ExtrudeMode::Boss, b.block, EndCondition::Blind, true));
        CAD_CHECK(!r.ok && r.field == "cutOutside");
    }
}

void testModosEPersistencia()
{
    std::printf("P4: modos, união, sentido, persistência e exportação\n");
    Base b = base();
    // Adicionar material com união (saliência).
    const std::string boss = extrude(b.doc, params(b.circle, 10.0, ExtrudeMode::Boss, b.block));
    CAD_CHECK_NEAR(ShapeInspector::volume(b.doc.shape()), kBox + kPi * 1000.0, 1e-3);
    CAD_CHECK(b.doc.solidCount() == 1);
    // Sem união (novo corpo): mesmo perfil, corpo independente.
    const std::string body = extrude(b.doc, params(b.circle, 10.0, ExtrudeMode::NewBody));
    CAD_CHECK(b.doc.solidCount() == 2);
    // Inversão de direção do novo corpo: para dentro da face (−Z), sobrepondo o bloco.
    CAD_CHECK(b.doc.updateExtrusion(body, params(b.circle, 10.0, ExtrudeMode::NewBody, {}, EndCondition::Blind, false, true)).ok);
    CAD_CHECK(inside(b.doc.findOperation(body)->result, 50, 30, 15));
    // Corte externo atravessando o corpo com a saliência.
    const std::string cut = extrude(b.doc, params(b.circle, 10.0, ExtrudeMode::Cut, boss, EndCondition::ThroughAll, true));
    const double cutVolume = ShapeInspector::volume(b.doc.findOperation(cut)->result);
    // Cilindro passante R 10: corpo do bloco (20) + saliência (10) dentro do perfil.
    CAD_CHECK_NEAR(cutVolume, kPi * 100.0 * 30.0, 1e-3);
    // Persistência (esquema 8, cutOutside) e reabertura idêntica.
    const QByteArray json = DocumentFile::toJson(b.doc);
    CAD_CHECK(json.contains("\"cutOutside\": true"));
    CAD_CHECK(json.contains(QByteArray("\"schemaVersion\": ") + QByteArray::number(DocumentFile::kSchemaVersion)));
    CadDocument reopened;
    const FileResult r = DocumentFile::fromJson(json, reopened);
    CAD_CHECK(r.ok);
    CAD_CHECK(reopened.findOperation(cut) && reopened.findOperation(cut)->cutOutside);
    CAD_CHECK_NEAR(ShapeInspector::volume(reopened.shape()), ShapeInspector::volume(b.doc.shape()), 1e-6);
    // Esquema 7 não aceita o campo novo.
    QByteArray v7 = json;
    v7.replace(QByteArray("\"schemaVersion\": ") + QByteArray::number(DocumentFile::kSchemaVersion), "\"schemaVersion\": 7");
    CadDocument old;
    CAD_CHECK(!DocumentFile::fromJson(v7, old).ok);
    // Exportação STEP/STL do resultado.
    const fs::path dir = fs::temp_directory_path() / "cadmecanico_e57";
    fs::create_directories(dir);
    const fs::path step = dir / "corte_externo.step";
    CAD_CHECK(cad::io::StepExporter().write(b.doc.shape(), step).ok);
    const auto back = cad::io::StepImporter().read(step);
    CAD_CHECK(back.ok);
    CAD_CHECK_NEAR(ShapeInspector::volume(back.shape), ShapeInspector::volume(b.doc.shape()), 1e-3);
    const auto stl = cad::io::StlExporter().write(b.doc.shape(), dir / "corte_externo.stl");
    CAD_CHECK(stl.ok && stl.triangles > 0);
    std::printf("  STEP %.4f mm3; STL %d triangulos\n", ShapeInspector::volume(back.shape), stl.triangles);
}

// --- P2 --------------------------------------------------------------------------

void testDiagnostico()
{
    std::printf("P2: diagnóstico e abertura com erro\n");
    Base b = base();
    // Fillet 3D (op-2) na aresta superior frontal do bloco.
    FinishParameters f;
    f.sourceId = b.block;
    f.value = 2.0;
    f.edges = b.doc.edgeSignatures(b.block, {edgeAt(b.doc.findOperation(b.block)->result, 50, 0, 20)});
    std::string fillet;
    CAD_CHECK(b.doc.addFinish(OperationType::Fillet3D, f, &fillet).ok);
    // Corte passante no resultado do fillet (depende dele) e novo corpo independente.
    const std::string cut = extrude(b.doc, params(b.circle, 10.0, ExtrudeMode::Cut, fillet, EndCondition::ThroughAll));
    const std::string other = extrude(b.doc, params(sketchWith(b.doc, PlaneType::XY, {SketchRectangle{200, 0, 10, 10}}), 5.0));
    // Documento válido: sem erros.
    CAD_CHECK(!b.doc.hasInvalidOperations());
    CAD_CHECK(count(diagnose(b.doc), DiagnosticSeverity::Error) == 0);
    const double validVolume = ShapeInspector::volume(b.doc.shape());

    // Erro de reconstrução numa edição: recusada, documento intacto.
    FinishParameters big = b.doc.finishParameters(fillet);
    big.value = 50.0;
    const int revision = b.doc.revision();
    const auto refused = b.doc.updateFinish(fillet, big);
    std::printf("  edicao recusada: %s\n", refused.message.c_str());
    CAD_CHECK(!refused.ok);
    CAD_CHECK(b.doc.revision() == revision && b.doc.finishParameters(fillet).value == 2.0);

    // Arquivo com o raio inviável (gravado por outra versão/edição externa).
    const QByteArray broken = withParameter(DocumentFile::toJson(b.doc), QString::fromStdString(fillet), "radius", 50.0);
    CadDocument strict;
    CAD_CHECK(!DocumentFile::fromJson(broken, strict).ok); // modo estrito (padrão) recusa
    CadDocument opened;
    const FileResult r = DocumentFile::fromJson(broken, opened, true);
    std::printf("  abertura com erro: %s\n", r.message.toUtf8().constData());
    CAD_CHECK(r.ok && r.message.contains("inválida"));
    CAD_CHECK(opened.hasInvalidOperations());
    CAD_CHECK(opened.operations().size() == b.doc.operations().size()); // nada descartado
    CAD_CHECK(opened.finishParameters(fillet).value == 50.0);           // nada alterado
    CAD_CHECK(opened.findOperation(fillet)->status == OperationStatus::Invalid);
    CAD_CHECK(opened.findOperation(cut)->status == OperationStatus::Invalid);
    CAD_CHECK(opened.findOperation(cut)->blockedBy == fillet);
    CAD_CHECK(opened.findOperation(other)->status == OperationStatus::Valid); // independente
    CAD_CHECK(opened.findOperation(b.block)->status == OperationStatus::Valid);
    // Geometria exibida: bloco (último estado válido da cadeia) + corpo independente.
    CAD_CHECK_NEAR(ShapeInspector::volume(opened.shape()), kBox + 500.0, 1e-3);
    const auto list = diagnose(opened);
    CAD_CHECK(count(list, DiagnosticSeverity::Error) == 2);
    const Diagnostic* df = findDiagnostic(list, fillet);
    CAD_CHECK(df && df->state == "Inválida" && df->cause.find("Raio grande demais") != std::string::npos
              && df->guidance.find("Reduza") != std::string::npos && !df->description.empty());
    const Diagnostic* dc = findDiagnostic(list, cut);
    CAD_CHECK(dc && dc->state == "Inválida (bloqueada)" && dc->guidance.find(fillet) != std::string::npos);
    if (df)
        std::printf("  [%s] %s: %s | causa: %s | correcao: %s\n", severityLabel(df->severity), df->itemName.c_str(),
                    df->description.c_str(), df->cause.c_str(), df->guidance.c_str());

    // Persistência do documento com erro: salvo sem alterar o raio; reaberto com o mesmo estado.
    const QByteArray resaved = DocumentFile::toJson(opened);
    CadDocument again;
    CAD_CHECK(DocumentFile::fromJson(resaved, again, true).ok);
    CAD_CHECK(again.finishParameters(fillet).value == 50.0 && again.hasInvalidOperations());

    // Enquanto houver erro: operação nova independente é aceita; operação nova que falha, não.
    const std::string extra = extrude(opened, params(sketchWith(opened, PlaneType::XY, {SketchCircle{300, 0, 5}}), 5.0));
    CAD_CHECK(opened.findOperation(extra)->status == OperationStatus::Valid);
    const auto bad = opened.addExtrusion(params(opened.findSketch("sk-1") ? "sk-1" : b.circle, 10.0, ExtrudeMode::Boss,
                                                other, EndCondition::Blind, false, true));
    CAD_CHECK(!bad.ok);

    // Correção de parâmetros: raio viável → cadeia recalculada e documento válido (recuperação).
    FinishParameters fixedParams = opened.finishParameters(fillet);
    fixedParams.value = 3.0;
    const auto fixedResult = opened.updateFinish(fillet, fixedParams);
    std::printf("  correcao: %s\n", fixedResult.ok ? "aplicada" : fixedResult.message.c_str());
    CAD_CHECK(fixedResult.ok);
    CAD_CHECK(!opened.hasInvalidOperations());
    CAD_CHECK(count(diagnose(opened), DiagnosticSeverity::Error) == 0);
    CAD_CHECK(opened.findOperation(cut)->status == OperationStatus::Valid);
    CAD_CHECK(ShapeInspector::volume(opened.shape()) < validVolume + kPi * 25.0 * 5.0 + 1.0);
    // Documento corrigido volta ao modo estrito: nova falha é recusada.
    FinishParameters again50 = opened.finishParameters(fillet);
    again50.value = 50.0;
    CAD_CHECK(!opened.updateFinish(fillet, again50).ok);
}

void testAvisos()
{
    std::printf("P2: avisos\n");
    CadDocument doc;
    const std::string empty = sketchWith(doc, PlaneType::XY, {});
    const std::string open = sketchWith(doc, PlaneType::XY, {SketchLine{0, 0, 10, 0}, SketchLine{10, 0, 10, 10}});
    const std::string closed = sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 10, 10}});
    const auto list = diagnose(doc);
    CAD_CHECK(count(list, DiagnosticSeverity::Error) == 0);
    CAD_CHECK(count(list, DiagnosticSeverity::Warning) == 2);
    CAD_CHECK(findDiagnostic(list, empty) && findDiagnostic(list, empty)->description == "Esboço vazio.");
    CAD_CHECK(findDiagnostic(list, open) && findDiagnostic(list, open)->description.find("perfil fechado") != std::string::npos);
    CAD_CHECK(!findDiagnostic(list, closed));
    // Explicações por tipo de falha.
    CAD_CHECK(explainFailure(OperationType::Chamfer3D, "aresta ambígua: 2 candidatas").cause.find("Mais de uma") == 0);
    CAD_CHECK(explainFailure(OperationType::Extrude, "face de suporte não remapeável").guidance.find("esboço") != std::string::npos);
    CAD_CHECK(explainFailure(OperationType::Extrude, "a ferramenta de corte não intersecta").guidance.find("sentido")
              != std::string::npos);
    // Corte externo sem material (sessão real da E5.7): não é falha do perfil.
    CAD_CHECK(explainFailure(OperationType::Extrude, "não há material fora do perfil na extensão do corte (op-3)")
                  .cause.find("não alcança") != std::string::npos);
    CAD_CHECK(explainFailure(OperationType::Extrude, "perfil do Esboço 2 inválido: Perfil aberto: extremidade livre")
                  .cause.find("contorno fechado") != std::string::npos);
}

void testEsbocoSemSuporte()
{
    std::printf("P2: esboço sobre face sem suporte recuperável\n");
    Base b = base();
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(b.doc)).object();
    QJsonObject doc = root.value("document").toObject();
    QJsonArray sketches = doc.value("sketches").toArray();
    for (int i = 0; i < sketches.size(); ++i)
    {
        QJsonObject s = sketches[i].toObject();
        if (s.value("id").toString() != QString::fromStdString(b.circle))
            continue;
        QJsonObject support = s.value("support").toObject();
        QJsonObject face = support.value("face").toObject();
        face.insert("normal", QJsonArray{0.6, 0.0, 0.8}); // nenhuma face do bloco tem essa normal
        support.insert("face", face);
        s.insert("support", support);
        sketches[i] = s;
    }
    doc.insert("sketches", sketches);
    root.insert("document", doc);
    const QByteArray json = QJsonDocument(root).toJson();
    CadDocument strict;
    CAD_CHECK(!DocumentFile::fromJson(json, strict).ok);
    CadDocument opened;
    CAD_CHECK(DocumentFile::fromJson(json, opened, true).ok);
    CAD_CHECK(opened.sketchIssues().size() == 1);
    const auto list = diagnose(opened);
    const Diagnostic* d = findDiagnostic(list, b.circle);
    CAD_CHECK(d && d->severity == DiagnosticSeverity::Error && d->description.find("remapeável") != std::string::npos);
    CAD_CHECK_NEAR(ShapeInspector::volume(opened.shape()), kBox, 1e-6);
}

void testFaceInclinadaDeChanfro()
{
    std::printf("P1: esboço sobre face inclinada gerada por chanfro (normal externa)\n");
    Base b = base();
    FinishParameters c;
    c.sourceId = b.block;
    c.value = 10.0;
    c.edges = b.doc.edgeSignatures(b.block, {edgeAt(b.doc.findOperation(b.block)->result, 50, 0, 20)});
    std::string chamfer;
    CAD_CHECK(b.doc.addFinish(OperationType::Chamfer3D, c, &chamfer).ok);
    const TopoDS_Shape body = b.doc.findOperation(chamfer)->result;
    int checked = 0;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        const auto d = cad::modeling::describeFace(f);
        if (std::fabs(d.normal.Z()) < 0.1 || std::fabs(d.normal.Y()) < 0.1)
            continue; // só a face inclinada
        std::printf("  face inclinada: orientacao %d, normal (%.3f; %.3f; %.3f)\n", int(f.Orientation()), d.normal.X(),
                    d.normal.Y(), d.normal.Z());
        // Mesma face com a orientação trocada (como pode chegar da seleção do visualizador).
        for (const TopoDS_Shape& candidate : {TopoDS_Shape(f), f.Reversed()})
        {
            std::string error;
            const auto s = b.doc.createFaceSketch(chamfer, candidate, error);
            CAD_CHECK(s.has_value());
            if (s)
            {
                const gp_Dir n = s->plane().normal();
                std::printf("    esboco: normal (%.3f; %.3f; %.3f)\n", n.X(), n.Y(), n.Z());
                CAD_CHECK(n.IsEqual(gp_Dir(0, -1, 1), 1e-6)); // externa: para fora do sólido
            }
        }
        ++checked;
    }
    CAD_CHECK(checked == 1);
}

} // namespace

int main()
{
    testFaceInclinadaDeChanfro();
    testCorteExterno();
    testModosEPersistencia();
    testDiagnostico();
    testAvisos();
    testEsbocoSemSuporte();
    return cad::test::finish("test_e57");
}
