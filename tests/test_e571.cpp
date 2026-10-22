// E5.7.1 sem interface.
//  P3 — referências topológicas: nomes das faces (origem + papel) propagados pelo histórico do
//       OCCT; esboço sobre face e Fillet/Chamfer reconstruídos pelo nome quando a geometria
//       sozinha é ambígua ou não reconhece a referência; dimensões/profundidades/booleanas
//       alteradas; persistência (nome no arquivo, documentos antigos sem nome); ambiguidade e
//       referências não recuperáveis recusadas.
//  P5 — condições de extrusão: plano médio, direção 2, atravessar tudo nos dois sentidos, até a
//       próxima face (corte e saliência), até a face (paralela e inclinada; troca da face;
//       alteração da geometria da face), persistência e STEP/STL.
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "document/Diagnostics.h"
#include "document/DocumentFile.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "io/StlExporter.h"
#include "modeling/ShapeInspector.h"
#include "modeling/TopoNaming.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Bnd_Box.hxx>
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

std::string sketchWith(CadDocument& doc, PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s = doc.createSketch(plane);
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s.addEntity(g).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

ExtrudeParameters params(const std::string& sketch, double depth, ExtrudeMode mode = ExtrudeMode::NewBody,
                         const std::string& target = {}, EndCondition end = EndCondition::Blind)
{
    ExtrudeParameters p;
    p.sketchId = sketch;
    p.depth = depth;
    p.mode = mode;
    p.targetId = target;
    p.endCondition = end;
    return p;
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

TopoDS_Face faceWith(const TopoDS_Shape& body, const gp_Dir& normal, const gp_Pnt& near)
{
    TopoDS_Face best;
    double bestD = 1e300;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        const auto pl = cad::modeling::planarFacePlane(f);
        if (!pl || pl->Axis().Direction().Angle(normal) > 1e-6)
            continue;
        const double d = cad::modeling::describeFace(f).centroid.Distance(near);
        if (d < bestD)
        {
            bestD = d;
            best = f;
        }
    }
    CAD_CHECK(!best.IsNull());
    return best;
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

TopoDS_Edge edgeNear(const TopoDS_Shape& shape, const gp_Pnt& p, double tol = 1e-6)
{
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(p) < tol)
            return TopoDS::Edge(ex.Current());
    }
    std::printf("  aresta nao encontrada (%.2f; %.2f; %.2f)\n", p.X(), p.Y(), p.Z());
    CAD_CHECK(false);
    return TopoDS_Edge();
}

bool inside(const TopoDS_Shape& s, double x, double y, double z)
{
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-7);
    return c.State() == TopAbs_IN;
}

Bnd_Box boxOf(const TopoDS_Shape& s)
{
    Bnd_Box b;
    BRepBndLib::Add(s, b, false);
    return b;
}

void report(const char* what, double got, double expected, double tol = 1e-3)
{
    std::printf("  %s: %.4f (esperado %.4f)\n", what, got, expected);
    CAD_CHECK_NEAR(got, expected, tol);
}

// Remove recursivamente as chaves "name" das referências (documento anterior à E5.7.1).
void stripNames(QJsonValueRef v);
void stripNames(QJsonObject& o)
{
    for (auto it = o.begin(); it != o.end();)
    {
        if (it.key() == QLatin1String("name") && (o.contains("normal") || o.contains("curve")))
        {
            it = o.erase(it);
            continue;
        }
        stripNames(it.value());
        ++it;
    }
}
void stripNames(QJsonValueRef v)
{
    if (v.isObject())
    {
        QJsonObject o = v.toObject();
        stripNames(o);
        v = o;
    }
    else if (v.isArray())
    {
        QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i)
            stripNames(a[i]);
        v = a;
    }
}

// --- P3 ------------------------------------------------------------------------------

void testNomes()
{
    std::printf("P3.1 Nomes das faces e arestas\n");
    CadDocument doc;
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    const TopoDS_Shape body = doc.findOperation(block)->result;
    const auto& names = doc.findOperation(block)->faceNames;
    CAD_CHECK(names.size() == 6);
    const std::string top = doc.faceName(block, faceWith(body, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)));
    const std::string base = doc.faceName(block, faceWith(body, gp_Dir(0, 0, -1), gp_Pnt(50, 30, 0)));
    const std::string front = doc.faceName(block, faceWith(body, gp_Dir(0, -1, 0), gp_Pnt(50, 0, 10)));
    std::printf("  topo=%s base=%s frente=%s\n", top.c_str(), base.c_str(), front.c_str());
    CAD_CHECK(top == "op-1:topo" && base == "op-1:base");
    CAD_CHECK(front.rfind("op-1:lado:e-1:", 0) == 0);
    const std::string edge = doc.edgeName(block, edgeNear(body, gp_Pnt(50, 0, 20)));
    std::printf("  aresta superior frontal=%s\n", edge.c_str());
    CAD_CHECK(edge == front + "|op-1:topo" || edge == "op-1:topo|" + front);
    // Furo passante: parede nomeada; faces do bloco mantêm os nomes pelo histórico.
    CadDocument part = CadDocument::withTestPart();
    const auto* hole = part.findOperation("op-2");
    int wall = 0, kept = 0;
    for (const auto& [face, name] : hole->faceNames.items())
    {
        wall += name == "op-2:furo" ? 1 : 0;
        kept += name.rfind("op-1:", 0) == 0 ? 1 : 0;
    }
    std::printf("  furo: %d parede(s) 'op-2:furo', %d faces do bloco\n", wall, kept);
    CAD_CHECK(wall >= 1 && kept == 6);
}

// Saliência com face lateral paralela à face lateral do bloco: a geometria sozinha não decide.
void testSuporteLateralAmbiguoPelaGeometria()
{
    std::printf("P3.2 Esboço na face lateral de uma saliência; largura alterada (geometria ambígua)\n");
    CadDocument doc;
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    const std::string top =
        faceSketch(doc, block, faceWith(doc.findOperation(block)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                   {SketchRectangle{40, 10, 20, 40}});
    const std::string boss = extrude(doc, params(top, 20, ExtrudeMode::Boss, block));
    // Esboço sobre a face lateral direita da saliência (x = 60) e corte de 5 mm a partir dela.
    const TopoDS_Face side = faceWith(doc.findOperation(boss)->result, gp_Dir(1, 0, 0), gp_Pnt(60, 30, 30));
    const std::string sideSketch = faceSketch(doc, boss, side, {});
    {
        // Círculo no centro da face (coordenadas locais da face).
        Sketch s = *doc.findSketch(sideSketch);
        const gp_Pnt2d c = s.plane().toLocal(gp_Pnt(60, 30, 30));
        CAD_CHECK(s.addEntity(SketchCircle{c.X(), c.Y(), 4}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
    }
    const std::string cut = extrude(doc, params(sideSketch, 5, ExtrudeMode::Cut, boss));
    const auto reference = doc.findSketch(sideSketch)->support()->face;
    std::printf("  referencia: %s\n", reference.name.c_str());
    CAD_CHECK(reference.name.rfind(boss + ":lado:e-1:", 0) == 0);
    // Saliência mais larga: a face direita vai para x = 90 (a lateral do bloco está em x = 100).
    Sketch wide = *doc.findSketch(top);
    CAD_CHECK(wide.updateEntity("e-1", SketchRectangle{40, 10, 50, 40}).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(wide, error));
    if (!error.empty())
        std::printf("  erro: %s\n", error.c_str());
    // Somente a geometria (assinatura antiga sem nome) seria ambígua entre x = 90 e x = 100.
    cad::modeling::FaceSignature legacy = reference;
    legacy.name.clear();
    const auto geometric = cad::modeling::resolvePlanarFace(doc.findOperation(boss)->result, legacy);
    std::printf("  so geometria: %s\n", geometric.ok ? "resolvida" : geometric.message.c_str());
    CAD_CHECK(!geometric.ok && geometric.message.find("ambígua") != std::string::npos);
    // Pelo nome: o esboço acompanha a face (plano em x = 90) e o corte está nela.
    const gp_Pnt origin = doc.findSketch(sideSketch)->plane().origin();
    std::printf("  plano do esboco: x = %.3f\n", origin.X());
    CAD_CHECK(std::fabs(origin.X() - 90.0) < 1e-6);
    CAD_CHECK(doc.findOperation(cut)->status == OperationStatus::Valid);
    CAD_CHECK(!inside(doc.shape(), 88, 30, 30) && inside(doc.shape(), 84, 30, 30) && inside(doc.shape(), 88, 30, 36));
    CAD_CHECK(doc.findOperation(cut)->notes.empty() && doc.sketchNotes().empty());
}

// Altura do bloco alterada (dimensões do sólido) com esboço na face superior e saliência.
void testDimensoesEProfundidade()
{
    std::printf("P3.3 Dimensões do sólido e profundidade alteradas\n");
    CadDocument doc;
    const std::string base = sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}});
    const std::string block = extrude(doc, params(base, 20));
    const std::string topSketch =
        faceSketch(doc, block, faceWith(doc.findOperation(block)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                   {SketchCircle{20, 30, 8}});
    const std::string boss = extrude(doc, params(topSketch, 20, ExtrudeMode::Boss, block));
    const std::string holeSketch =
        faceSketch(doc, boss, faceWith(doc.findOperation(boss)->result, gp_Dir(0, 0, 1), gp_Pnt(60, 30, 20)),
                   {SketchCircle{70, 30, 5}});
    const std::string hole = extrude(doc, params(holeSketch, 5, ExtrudeMode::Cut, boss));
    // Profundidade do bloco 20 → 30: face superior sobe; esboços acompanham pelo nome.
    ExtrudeParameters deeper = doc.extrudeParameters(block);
    deeper.depth = 30;
    const auto r = doc.updateExtrusion(block, deeper);
    if (!r.ok)
        std::printf("  erro: %s\n", r.message.c_str());
    CAD_CHECK(r.ok);
    CAD_CHECK(std::fabs(doc.findSketch(topSketch)->plane().origin().Z() - 30.0) < 1e-9);
    CAD_CHECK(std::fabs(doc.findSketch(holeSketch)->plane().origin().Z() - 30.0) < 1e-9);
    report("volume", ShapeInspector::volume(doc.shape()), 100.0 * 60 * 30 + kPi * 64 * 20 - kPi * 25 * 5);
    CAD_CHECK(!inside(doc.shape(), 70, 30, 27) && inside(doc.shape(), 70, 30, 23));
    // Geometria anterior ao esboço alterada (esboço base: bloco mais curto em X).
    Sketch shorter = *doc.findSketch(base);
    CAD_CHECK(shorter.updateEntity("e-1", SketchRectangle{0, 0, 90, 60}).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(shorter, error));
    report("volume (bloco 90 mm)", ShapeInspector::volume(doc.shape()), 90.0 * 60 * 30 + kPi * 64 * 20 - kPi * 25 * 5);
    CAD_CHECK(doc.findOperation(hole)->status == OperationStatus::Valid);
}

// Fillet na aresta entre o topo e a face inclinada de uma saliência triangular; o vértice do
// triângulo muda (a normal da face inclinada muda): a geometria não reconhece mais a aresta.
void testFilletAposMudancaDeNormal()
{
    std::printf("P3.4 Fillet reconstruído após mudança da normal da face adjacente\n");
    CadDocument doc;
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    const std::string tri = faceSketch(
        doc, block, faceWith(doc.findOperation(block)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
        {SketchLine{10, 10, 50, 10}, SketchLine{50, 10, 30, 40}, SketchLine{30, 40, 10, 10}});
    const std::string boss = extrude(doc, params(tri, 10, ExtrudeMode::Boss, block));
    FinishParameters f;
    f.sourceId = boss;
    f.value = 2.0;
    f.edges = doc.edgeSignatures(boss, {edgeNear(doc.findOperation(boss)->result, gp_Pnt(40, 25, 30))});
    std::printf("  aresta: %s\n", f.edges.front().name.c_str());
    CAD_CHECK(f.edges.front().name.find(boss + ":lado:e-2") != std::string::npos
              && f.edges.front().name.find(boss + ":topo") != std::string::npos);
    std::string fillet;
    CAD_CHECK(doc.addFinish(OperationType::Fillet3D, f, &fillet).ok);
    // Vértice (30, 40) → (40, 45): a face de e-2 muda de normal.
    Sketch moved = *doc.findSketch(tri);
    CAD_CHECK(moved.updateEntity("e-2", SketchLine{50, 10, 40, 45}).ok);
    CAD_CHECK(moved.updateEntity("e-3", SketchLine{40, 45, 10, 10}).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(moved, error));
    if (!error.empty())
        std::printf("  erro: %s\n", error.c_str());
    cad::modeling::EdgeSignature legacy = doc.finishParameters(fillet).edges.front();
    // Assinatura geométrica anterior (normais antigas) sem nome: não remapeável.
    cad::modeling::EdgeSignature old = f.edges.front();
    old.name.clear();
    const auto geometric = cad::modeling::resolveEdge(doc.findOperation(boss)->result, old);
    std::printf("  so geometria: %s\n", geometric.ok ? "resolvida" : geometric.message.c_str());
    CAD_CHECK(!geometric.ok);
    CAD_CHECK(doc.findOperation(fillet)->status == OperationStatus::Valid);
    std::printf("  aresta remapeada: meio (%.2f; %.2f; %.2f)\n", legacy.mid.X(), legacy.mid.Y(), legacy.mid.Z());
    CAD_CHECK(legacy.mid.Distance(gp_Pnt(45, 27.5, 30)) < 1e-6);
    CAD_CHECK(doc.findOperation(fillet)->notes.empty());
}

// Booleana entre corpos e Chamfer na aresta do resultado; operando alterado.
void testBooleanaEChanfro()
{
    std::printf("P3.5 Booleana e Chamfer 3D com operando alterado\n");
    CadDocument doc;
    const std::string a = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    const std::string bSketch = sketchWith(doc, PlaneType::XY, {SketchRectangle{80, 20, 40, 20}});
    const std::string b = extrude(doc, params(bSketch, 30));
    BooleanParameters u;
    u.kind = BooleanKind::Fuse;
    u.operandA = a;
    u.operandB = b;
    std::string fused;
    CAD_CHECK(doc.addBoolean(u, &fused).ok);
    FinishParameters c;
    c.sourceId = fused;
    c.value = 3.0;
    c.edges = doc.edgeSignatures(fused, {edgeNear(doc.findOperation(fused)->result, gp_Pnt(50, 0, 20))});
    std::printf("  aresta: %s\n", c.edges.front().name.c_str());
    std::string chamfer;
    CAD_CHECK(doc.addFinish(OperationType::Chamfer3D, c, &chamfer).ok);
    const double before = ShapeInspector::volume(doc.shape());
    // Operando B maior e deslocado: a união muda; o chanfro continua na mesma aresta.
    Sketch moved = *doc.findSketch(bSketch);
    CAD_CHECK(moved.updateEntity("e-1", SketchRectangle{70, 10, 60, 30}).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(moved, error));
    CAD_CHECK(doc.findOperation(chamfer)->status == OperationStatus::Valid);
    const double after = ShapeInspector::volume(doc.shape());
    // Volume adicionado pela mudança de B: (60·30 − 20·20 [parte dentro de A antiga] ...) — conferido por
    // diferença dos corpos: A ∪ B' − A ∪ B, chanfro igual nos dois casos.
    const double unionBefore = 100.0 * 60 * 20 + 40.0 * 20 * 30 - 20.0 * 20 * 20;
    const double unionAfter = 100.0 * 60 * 20 + 60.0 * 30 * 30 - 30.0 * 30 * 20;
    report("variação de volume", after - before, unionAfter - unionBefore);
    CAD_CHECK(!inside(doc.shape(), 50, 0.5, 19.5));
}

void testPersistenciaELegado()
{
    std::printf("P3.6 Persistência dos nomes e documento antigo sem nomes\n");
    CadDocument doc;
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    const std::string top =
        faceSketch(doc, block, faceWith(doc.findOperation(block)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                   {SketchCircle{50, 30, 10}});
    const std::string cut = extrude(doc, params(top, 10, ExtrudeMode::Cut, block, EndCondition::ThroughAll));
    FinishParameters f;
    f.sourceId = cut;
    f.value = 2.0;
    f.edges = doc.edgeSignatures(cut, {edgeNear(doc.findOperation(cut)->result, gp_Pnt(50, 0, 20))});
    CAD_CHECK(doc.addFinish(OperationType::Fillet3D, f).ok);
    const QByteArray json = DocumentFile::toJson(doc);
    CAD_CHECK(json.contains("\"name\": \"op-1:topo\""));
    CAD_CHECK(json.contains("|op-1:topo"));
    CadDocument reopened;
    CAD_CHECK(DocumentFile::fromJson(json, reopened).ok);
    report("reaberto", ShapeInspector::volume(reopened.shape()), ShapeInspector::volume(doc.shape()), 1e-6);
    // Documento antigo (esquema 8, sem nomes): abre pela geometria e recebe os nomes.
    QJsonObject root = QJsonDocument::fromJson(json).object();
    root["schemaVersion"] = 8;
    QJsonObject d = root["document"].toObject();
    QJsonValueRef sketches = d["sketches"];
    stripNames(sketches);
    QJsonValueRef ops = d["operations"];
    stripNames(ops);
    root["document"] = d;
    const QByteArray legacy = QJsonDocument(root).toJson();
    CAD_CHECK(!legacy.contains("op-1:topo"));
    CadDocument old;
    const FileResult r = DocumentFile::fromJson(legacy, old);
    if (!r.ok)
        std::printf("  erro: %s\n", r.message.toUtf8().constData());
    CAD_CHECK(r.ok);
    CAD_CHECK(old.findSketch(top)->support()->face.name == "op-1:topo");
    report("legado", ShapeInspector::volume(old.shape()), ShapeInspector::volume(doc.shape()), 1e-6);
    // Nome topológico perdido, face no mesmo plano: recuperada pela geometria com AVISO no
    // diagnóstico (não é associada em silêncio).
    QByteArray renamed = json;
    renamed.replace("\"name\": \"op-1:topo\"", "\"name\": \"op-1:antigo\"");
    CadDocument warned;
    CAD_CHECK(DocumentFile::fromJson(renamed, warned).ok);
    CAD_CHECK(warned.sketchNotes().size() == 1);
    const auto list = diagnose(warned);
    const auto w = std::find_if(list.begin(), list.end(), [&](const Diagnostic& d) {
        return d.itemId == top && d.severity == DiagnosticSeverity::Warning;
    });
    CAD_CHECK(w != list.end() && w->description.find("geometria") != std::string::npos);
    if (w != list.end())
        std::printf("  aviso: %s\n", w->description.c_str());
    CAD_CHECK(warned.findSketch(top)->support()->face.name == "op-1:topo"); // referência atualizada
}

void testReferenciasIncorretasRecusadas()
{
    std::printf("P3.7 Ambiguidade e referência não recuperável\n");
    // Face superior dividida em duas partes simétricas por uma ranhura: o mesmo nome em duas
    // faces; referência no meio das duas = ambígua (nunca escolhe uma em silêncio).
    CadDocument doc;
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    const std::string slotSketch =
        faceSketch(doc, block, faceWith(doc.findOperation(block)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                   {SketchRectangle{45, -5, 10, 70}});
    const std::string slot = extrude(doc, params(slotSketch, 5, ExtrudeMode::Cut, block));
    const auto* op = doc.findOperation(slot);
    const auto halves = op->faceNames.facesNamed(op->result, "op-1:topo");
    std::printf("  partes 'op-1:topo': %zu\n", halves.size());
    CAD_CHECK(halves.size() == 2);
    cad::modeling::FaceSignature middle = cad::modeling::describeFace(halves.front());
    middle.name = "op-1:topo";
    middle.centroid = gp_Pnt(50, 30, 20);
    const auto amb = cad::modeling::resolveNamedFace(op->result, op->faceNames, middle);
    std::printf("  meio: %s\n", amb.ok ? "resolvida" : amb.message.c_str());
    CAD_CHECK(!amb.ok && amb.message.find("ambígua") != std::string::npos);
    // Perto de uma das partes: desempate seguro pela geometria.
    middle.centroid = gp_Pnt(20, 30, 20);
    const auto left = cad::modeling::resolveNamedFace(op->result, op->faceNames, middle);
    CAD_CHECK(left.ok && left.byName && cad::modeling::describeFace(left.face).centroid.X() < 45);
    // Nome inexistente e sem face no mesmo plano: não recuperável (não associa a outra face).
    cad::modeling::FaceSignature lost = middle;
    lost.name = "op-1:inexistente";
    lost.offset = 25.0;
    lost.centroid = gp_Pnt(20, 30, 25);
    const auto none = cad::modeling::resolveNamedFace(op->result, op->faceNames, lost);
    std::printf("  inexistente: %s\n", none.ok ? "resolvida" : none.message.c_str());
    CAD_CHECK(!none.ok && none.message.find("não remapeável") != std::string::npos);
    // Nome inexistente mas face no mesmo plano, sem ambiguidade (parte mais próxima com margem):
    // recuperada pela geometria (a reconstrução registra o aviso).
    lost.offset = 20.0;
    lost.centroid = gp_Pnt(20, 30, 20);
    const auto same = cad::modeling::resolveNamedFace(op->result, op->faceNames, lost);
    CAD_CHECK(same.ok && !same.byName && cad::modeling::describeFace(same.face).centroid.X() < 45);
}

// --- P5 ------------------------------------------------------------------------------

void testPlanoMedioEDirecao2()
{
    std::printf("P5.1 Plano médio e direção 2\n");
    CadDocument doc;
    const std::string sk = sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 10, 10}});
    const std::string mid = extrude(doc, params(sk, 20, ExtrudeMode::NewBody, {}, EndCondition::MidPlane));
    double x0, y0, z0, x1, y1, z1;
    boxOf(doc.findOperation(mid)->result).Get(x0, y0, z0, x1, y1, z1);
    std::printf("  plano medio: z %.3f..%.3f\n", z0, z1);
    CAD_CHECK(std::fabs(z0 + 10) < 1e-6 && std::fabs(z1 - 10) < 1e-6);
    report("volume plano medio", ShapeInspector::volume(doc.findOperation(mid)->result), 2000.0);
    ExtrudeParameters two = params(sk, 10);
    two.direction2 = true;
    two.depth2 = 5;
    CAD_CHECK(doc.updateExtrusion(mid, two).ok);
    boxOf(doc.findOperation(mid)->result).Get(x0, y0, z0, x1, y1, z1);
    std::printf("  duas direcoes: z %.3f..%.3f\n", z0, z1);
    CAD_CHECK(std::fabs(z0 + 5) < 1e-6 && std::fabs(z1 - 10) < 1e-6);
    report("volume duas direcoes", ShapeInspector::volume(doc.findOperation(mid)->result), 1500.0);
    // Validações.
    ExtrudeParameters bad = params(sk, 10, ExtrudeMode::NewBody, {}, EndCondition::MidPlane);
    bad.direction2 = true;
    CAD_CHECK(!CadDocument::validateExtrude(bad).ok);
    CAD_CHECK(!CadDocument::validateExtrude(params(sk, 10, ExtrudeMode::NewBody, {}, EndCondition::UpToNext)).ok);
    CAD_CHECK(!CadDocument::validateExtrude(params(sk, 10, ExtrudeMode::NewBody, {}, EndCondition::UpToFace)).ok);
}

void testAtravessarDoisSentidos()
{
    std::printf("P5.2 Corte atravessando tudo nos dois sentidos\n");
    CadDocument doc;
    const std::string block = extrude(
        doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{-50, -30, 100, 60}}), 20, ExtrudeMode::NewBody, {},
                    EndCondition::MidPlane)); // z −10..10
    const std::string hole = sketchWith(doc, PlaneType::XY, {SketchCircle{0, 0, 5}}); // plano z = 0, dentro do corpo
    const std::string one = extrude(doc, params(hole, 1, ExtrudeMode::Cut, block, EndCondition::ThroughAll));
    report("um sentido", ShapeInspector::volume(doc.shape()), 100.0 * 60 * 20 - kPi * 25 * 10);
    ExtrudeParameters both = params(hole, 1, ExtrudeMode::Cut, block, EndCondition::ThroughAllBoth);
    CAD_CHECK(doc.updateExtrusion(one, both).ok);
    report("dois sentidos", ShapeInspector::volume(doc.shape()), 100.0 * 60 * 20 - kPi * 25 * 20);
    CAD_CHECK(!inside(doc.shape(), 0, 0, 9) && !inside(doc.shape(), 0, 0, -9));
    // Saliência nos dois sentidos (adicionar material através do corpo).
    CadDocument d2;
    const std::string b2 = extrude(d2, params(sketchWith(d2, PlaneType::XY, {SketchRectangle{0, 0, 10, 10}}), 10));
    const std::string off = sketchWith(d2, PlaneType::XY, {SketchRectangle{20, 0, 10, 10}});
    const auto r = d2.addExtrusion(params(off, 1, ExtrudeMode::Boss, b2, EndCondition::ThroughAllBoth));
    std::printf("  saliencia dois sentidos sem contato: %s\n", r.ok ? "criada" : r.message.c_str());
    CAD_CHECK(!r.ok && r.message.find("não toca") != std::string::npos);
}

// Corpo com vão interno: placa inferior (z 0..10) e superior (z 20..30) unidas pelas
// extremidades (ranhura x 10..90, atravessando em Y).
std::string twoPlates(CadDocument& doc)
{
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 30));
    const TopoDS_Face front = faceWith(doc.findOperation(block)->result, gp_Dir(0, -1, 0), gp_Pnt(50, 0, 15));
    const std::string slotSketch = faceSketch(doc, block, front, {});
    Sketch s = *doc.findSketch(slotSketch);
    const gp_Pnt2d a = s.plane().toLocal(gp_Pnt(10, 0, 10));
    const gp_Pnt2d b = s.plane().toLocal(gp_Pnt(90, 0, 20));
    CAD_CHECK(s.addEntity(SketchRectangle::fromCorners(a.X(), a.Y(), b.X(), b.Y())).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return extrude(doc, params(slotSketch, 1, ExtrudeMode::Cut, block, EndCondition::ThroughAll));
}

void testAteAProxima()
{
    std::printf("P5.3 Até a próxima face\n");
    CadDocument doc;
    const std::string body = twoPlates(doc);
    const double base = ShapeInspector::volume(doc.shape());
    report("corpo com vão", base, 100.0 * 60 * 30 - 80.0 * 60 * 10);
    const std::string topSketch =
        faceSketch(doc, body, faceWith(doc.findOperation(body)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 30)),
                   {SketchCircle{50, 30, 5}});
    const std::string next = extrude(doc, params(topSketch, 1, ExtrudeMode::Cut, body, EndCondition::UpToNext));
    report("corte até a próxima (só a placa superior)", base - ShapeInspector::volume(doc.shape()), kPi * 25 * 10);
    CAD_CHECK(!inside(doc.shape(), 50, 30, 25) && inside(doc.shape(), 50, 30, 5));
    CAD_CHECK(doc.updateExtrusion(next, params(topSketch, 1, ExtrudeMode::Cut, body, EndCondition::ThroughAll)).ok);
    report("atravessar tudo (duas placas)", base - ShapeInspector::volume(doc.shape()), kPi * 25 * 20);
    // Saliência no vão: da face superior da placa inferior (z = 10) até a próxima face (z = 20).
    CadDocument d2;
    const std::string body2 = twoPlates(d2);
    const double base2 = ShapeInspector::volume(d2.shape());
    const std::string floor =
        faceSketch(d2, body2, faceWith(d2.findOperation(body2)->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 10)),
                   {SketchCircle{50, 30, 5}});
    extrude(d2, params(floor, 1, ExtrudeMode::Boss, body2, EndCondition::UpToNext));
    report("saliência até a próxima (vão de 10 mm)", ShapeInspector::volume(d2.shape()) - base2, kPi * 25 * 10);
    // Sem face à frente: recusada com mensagem.
    const std::string roof =
        faceSketch(d2, d2.activeBodies().front(),
                   faceWith(d2.findOperation(d2.activeBodies().front())->result, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 30)),
                   {SketchCircle{20, 30, 5}});
    const auto r = d2.addExtrusion(params(roof, 1, ExtrudeMode::Boss, d2.activeBodies().front(), EndCondition::UpToNext));
    std::printf("  saliencia sem face a frente: %s\n", r.message.c_str());
    CAD_CHECK(!r.ok && r.message.find("nenhuma face") != std::string::npos);
}

void testAteAFace()
{
    std::printf("P5.4 Até a face (paralela e inclinada; troca da face; geometria alterada)\n");
    CadDocument doc;
    const std::string block = extrude(doc, params(sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), 20));
    // Cada reconstrução gera formas novas: o resultado atual do bloco é buscado a cada uso.
    auto a = [&doc, &block] { return doc.findOperation(block)->result; };
    // Corpo B (novo corpo sobre a face superior, longe do pino): topo em z = 45.
    const std::string bSketch = faceSketch(doc, block, faceWith(a(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                           {SketchRectangle{60, 10, 30, 40}});
    const std::string b = extrude(doc, params(bSketch, 25));
    // Corpo C inclinado: triângulo na face lateral direita do bloco, extrudado para fora (+X); a
    // face inclinada está no plano z = 70 − y.
    const TopoDS_Face right = faceWith(a(), gp_Dir(1, 0, 0), gp_Pnt(100, 30, 10));
    const std::string triSketch = faceSketch(doc, block, right, {});
    {
        Sketch s = *doc.findSketch(triSketch);
        const gp_Pnt2d p1 = s.plane().toLocal(gp_Pnt(100, 10, 20));
        const gp_Pnt2d p2 = s.plane().toLocal(gp_Pnt(100, 50, 20));
        const gp_Pnt2d p3 = s.plane().toLocal(gp_Pnt(100, 10, 60));
        CAD_CHECK(s.addEntity(SketchLine{p1.X(), p1.Y(), p2.X(), p2.Y()}).ok);
        CAD_CHECK(s.addEntity(SketchLine{p2.X(), p2.Y(), p3.X(), p3.Y()}).ok);
        CAD_CHECK(s.addEntity(SketchLine{p3.X(), p3.Y(), p1.X(), p1.Y()}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
    }
    const std::string c = extrude(doc, params(triSketch, 10));
    // Pino: círculo R 5 em (20, 30) na face superior do bloco, saliência até a face superior de B.
    const std::string pin = faceSketch(doc, block, faceWith(a(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                       {SketchCircle{20, 30, 5}});
    const double before = ShapeInspector::volume(doc.findOperation(block)->result);
    ExtrudeParameters up = params(pin, 1, ExtrudeMode::Boss, block, EndCondition::UpToFace);
    up.limitBodyId = b;
    up.limitFace = *doc.faceReference(b, faceWith(doc.findOperation(b)->result, gp_Dir(0, 0, 1), gp_Pnt(75, 30, 45)));
    std::printf("  face limite: %s\n", up.limitFace.name.c_str());
    CAD_CHECK(up.limitFace.name == b + ":topo");
    const std::string boss = extrude(doc, up);
    report("até a face paralela (z = 45)", ShapeInspector::volume(doc.findOperation(boss)->result) - before, kPi * 25 * 25);
    // Geometria da face alterada (B mais alto: topo em z = 55): extensão recalculada.
    ExtrudeParameters taller = doc.extrudeParameters(b);
    taller.depth = 35;
    CAD_CHECK(doc.updateExtrusion(b, taller).ok);
    report("após B mais alto (z = 55)", ShapeInspector::volume(doc.findOperation(boss)->result) - before, kPi * 25 * 35);
    // Troca da face de referência: face inclinada de C (z = 70 − y; no eixo do pino y = 30 → 40).
    ExtrudeParameters inclined = doc.extrudeParameters(boss);
    inclined.limitBodyId = c;
    TopoDS_Face slanted;
    for (TopExp_Explorer ex(doc.findOperation(c)->result, TopAbs_FACE); ex.More(); ex.Next())
    {
        const auto pl = cad::modeling::planarFacePlane(TopoDS::Face(ex.Current()));
        if (pl && pl->Axis().Direction().Y() > 0.5 && pl->Axis().Direction().Z() > 0.5)
            slanted = TopoDS::Face(ex.Current());
    }
    CAD_CHECK(!slanted.IsNull());
    inclined.limitFace = *doc.faceReference(c, slanted);
    const auto r = doc.updateExtrusion(boss, inclined);
    if (!r.ok)
        std::printf("  erro: %s\n", r.message.c_str());
    CAD_CHECK(r.ok);
    // Volume do pino até o plano inclinado: área × altura no centroide (70 − 30 − 20 = 20).
    report("até a face inclinada", ShapeInspector::volume(doc.findOperation(boss)->result) - before, kPi * 25 * 20);
    // Face limite atrás do início: recusada.
    ExtrudeParameters behind = inclined;
    behind.reversed = true;
    CAD_CHECK(!doc.previewExtrusion(behind, boss).ok);

    // Persistência (esquema 9: upToFace + limitFace; direção 2) e STEP/STL.
    const QByteArray json = DocumentFile::toJson(doc);
    CAD_CHECK(json.contains("\"upToFace\"") && json.contains("\"limitFace\""));
    CadDocument reopened;
    const FileResult fr = DocumentFile::fromJson(json, reopened);
    if (!fr.ok)
        std::printf("  erro ao reabrir: %s\n", fr.message.toUtf8().constData());
    CAD_CHECK(fr.ok);
    report("reaberto", ShapeInspector::volume(reopened.shape()), ShapeInspector::volume(doc.shape()), 1e-6);
    const fs::path dir = fs::temp_directory_path() / "cadmecanico_e571";
    fs::create_directories(dir);
    CAD_CHECK(cad::io::StepExporter().write(doc.shape(), dir / "ate_a_face.step").ok);
    const auto back = cad::io::StepImporter().read(dir / "ate_a_face.step");
    CAD_CHECK(back.ok);
    report("STEP", ShapeInspector::volume(back.shape), ShapeInspector::volume(doc.shape()), 1e-3);
    const auto stl = cad::io::StlExporter().write(doc.shape(), dir / "ate_a_face.stl");
    CAD_CHECK(stl.ok && stl.triangles > 0);
    // Esquema 8 não aceita as condições novas.
    QByteArray v8 = json;
    v8.replace(QByteArray("\"schemaVersion\": ") + QByteArray::number(DocumentFile::kSchemaVersion), "\"schemaVersion\": 8");
    CadDocument old;
    CAD_CHECK(!DocumentFile::fromJson(v8, old).ok);
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progresso visível mesmo em falha grave
    testNomes();
    testSuporteLateralAmbiguoPelaGeometria();
    testDimensoesEProfundidade();
    testFilletAposMudancaDeNormal();
    testBooleanaEChanfro();
    testPersistenciaELegado();
    testReferenciasIncorretasRecusadas();
    testPlanoMedioEDirecao2();
    testAtravessarDoisSentidos();
    testAteAProxima();
    testAteAFace();
    return cad::test::finish("test_e571");
}
