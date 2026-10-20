// Documento paramétrico (E5.1): operações, identificadores, recálculo e validação (sem Qt).
#include "ExpectedTestPart.h"
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "modeling/ShapeInspector.h"
#include "modeling/TestPart.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace cad::document;
using cad::modeling::ShapeInspector;
using cad::modeling::ShapeSummary;

namespace {

constexpr double kPi = 3.14159265358979323846;

double expectedVolume(const PartParameters& p)
{
    return p.length * p.width * p.height - kPi * p.holeRadius * p.holeRadius * p.height;
}

// Peça com parâmetros p: sólido único válido, 7 faces, volume e caixa exatos.
void checkPart(const char* tag, const TopoDS_Shape& shape, const PartParameters& p)
{
    const ShapeSummary s = ShapeInspector::inspect(shape);
    std::printf("  [%s] volume %.4f (esperado %.4f), faces %d\n", tag, s.volume, expectedVolume(p), s.faces);
    CAD_CHECK(s.isValid);
    CAD_CHECK(s.solids == 1);
    CAD_CHECK(s.faces == 7);
    CAD_CHECK_NEAR(s.volume, expectedVolume(p), expectedVolume(p) * 1e-9);
    double x0, y0, z0, x1, y1, z1;
    s.boundingBox.Get(x0, y0, z0, x1, y1, z1);
    CAD_CHECK_NEAR(x0, 0.0, 1e-6);
    CAD_CHECK_NEAR(y0, 0.0, 1e-6);
    CAD_CHECK_NEAR(z0, 0.0, 1e-6);
    CAD_CHECK_NEAR(x1, p.length, 1e-6);
    CAD_CHECK_NEAR(y1, p.width, 1e-6);
    CAD_CHECK_NEAR(z1, p.height, 1e-6);
}

void checkRejected(CadDocument& doc, const PartParameters& bad, const char* field)
{
    const PartParameters before = doc.parameters();
    const TopoDS_Shape shapeBefore = doc.shape();
    const int revisionBefore = doc.revision();
    const bool modifiedBefore = doc.isModified();

    const ValidationResult r = doc.setParameters(bad);
    std::printf("  recusado (%s): %s\n", field, r.message.c_str());
    CAD_CHECK(!r.ok);
    CAD_CHECK(r.field == field);
    CAD_CHECK(!r.message.empty());
    // Último estado válido preservado integralmente.
    CAD_CHECK(doc.parameters() == before);
    CAD_CHECK(doc.shape().IsSame(shapeBefore));
    CAD_CHECK(doc.revision() == revisionBefore);
    CAD_CHECK(doc.isModified() == modifiedBefore);
    for (const Operation& op : doc.operations())
        CAD_CHECK(op.status == OperationStatus::Valid);
}

} // namespace

int main()
{
    // 0. Documento vazio (padrão desde a E5.2.1): sem operações, sem sólido, sem esboços.
    std::printf("0. Documento vazio\n");
    {
        CadDocument empty;
        CAD_CHECK(empty.name() == "Sem título");
        CAD_CHECK(!empty.hasPart());
        CAD_CHECK(empty.operations().empty());
        CAD_CHECK(empty.shape().IsNull());
        CAD_CHECK(empty.sketches().empty());
        CAD_CHECK(!empty.isModified());
        const ValidationResult r = empty.setParameters(PartParameters());
        CAD_CHECK(!r.ok && !r.message.empty());
        CAD_CHECK(!empty.hasPart() && empty.shape().IsNull() && !empty.isModified());
        CAD_CHECK(CadDocument("Outro").name() == "Outro" && !CadDocument("Outro").hasPart());
        // fromData sem peça: documento vazio.
        CadDocument out("x");
        std::string error;
        CAD_CHECK(CadDocument::fromData("Vazio", std::nullopt, "", "", out, error));
        CAD_CHECK(out.name() == "Vazio" && !out.hasPart() && out.shape().IsNull());
    }

    // 1. Documento com a peça de teste (criada explicitamente)
    std::printf("1. Documento com a peca de teste\n");
    CadDocument doc = CadDocument::withTestPart();
    CAD_CHECK(doc.hasPart());
    CAD_CHECK(doc.name() == "Peça de teste");
    CAD_CHECK(std::string(doc.unit()) == "mm");
    CAD_CHECK(!doc.isModified());
    CAD_CHECK(doc.revision() == 1);
    CAD_CHECK(doc.parameters() == PartParameters());

    // 2. Sequência de operações
    std::printf("2. Sequencia de operacoes\n");
    CAD_CHECK(doc.operations().size() == 2);
    const Operation& block = doc.operations().at(0);
    const Operation& hole = doc.operations().at(1);
    CAD_CHECK(block.type == OperationType::Block && block.name == "Bloco base");
    CAD_CHECK(hole.type == OperationType::ThroughHole && hole.name == "Furo passante");
    CAD_CHECK(block.parameter("length") == 100.0 && block.parameter("width") == 60.0 && block.parameter("height") == 20.0);
    CAD_CHECK(hole.parameter("radius") == 10.0);
    CAD_CHECK(block.status == OperationStatus::Valid && hole.status == OperationStatus::Valid);

    // 3. Identificadores estáveis
    std::printf("3. Identificadores estaveis\n");
    const std::string blockId = block.id, holeId = hole.id;
    CAD_CHECK(blockId == "op-1" && holeId == "op-2");
    CAD_CHECK(isValidIdentifier(blockId) && isValidIdentifier(holeId));
    CAD_CHECK(doc.findOperation("op-2") == &doc.operations().at(1));
    CAD_CHECK(doc.findOperation("inexistente") == nullptr);

    // 4. Geometria equivalente à peça original
    std::printf("4. Geometria equivalente a peca original\n");
    {
        const ShapeSummary s = ShapeInspector::inspect(doc.shape());
        cad::test::checkTestPart(s);
        const ShapeSummary ref = ShapeInspector::inspect(cad::modeling::buildTestPart());
        CAD_CHECK(s.faces == ref.faces && s.edges == ref.edges && s.vertices == ref.vertices);
        CAD_CHECK_NEAR(s.area, ref.area, 1e-6);
        CAD_CHECK_NEAR(s.volume, ref.volume, 1e-6);
    }

    // 5-8. Alteração de cada parâmetro
    struct Change { const char* name; PartParameters p; } changes[] = {
        {"5. comprimento", {150.0, 60.0, 20.0, 10.0}},
        {"6. largura", {150.0, 80.0, 20.0, 10.0}},
        {"7. altura", {150.0, 80.0, 35.0, 10.0}},
        {"8. raio do furo", {150.0, 80.0, 35.0, 15.5}},
    };
    int revision = doc.revision();
    for (const Change& c : changes)
    {
        std::printf("%s\n", c.name);
        const ValidationResult r = doc.setParameters(c.p);
        CAD_CHECK(r.ok);
        CAD_CHECK(doc.parameters() == c.p);
        CAD_CHECK(doc.revision() == ++revision);
        CAD_CHECK(doc.isModified());
        checkPart(c.name, doc.shape(), c.p);
        // Identificadores não mudam com a edição.
        CAD_CHECK(doc.operations().at(0).id == blockId && doc.operations().at(1).id == holeId);
    }
    // Furo continua centrado no bloco (relação geométrica).
    {
        const PartParameters p = doc.parameters();
        CadDocument shifted = CadDocument::withTestPart("x", p);
        const ShapeSummary s = ShapeInspector::inspect(shifted.shape());
        CAD_CHECK_NEAR(s.volume, expectedVolume(p), 1e-6);
    }

    // 9. Recálculo determinístico
    std::printf("9. Recalculo deterministico\n");
    {
        const PartParameters p{123.4, 56.7, 8.9, 12.3};
        CadDocument a = CadDocument::withTestPart("a", p), b = CadDocument::withTestPart("b", p);
        const ShapeSummary sa = ShapeInspector::inspect(a.shape());
        const ShapeSummary sb = ShapeInspector::inspect(b.shape());
        CAD_CHECK(sa.faces == sb.faces && sa.edges == sb.edges && sa.vertices == sb.vertices);
        CAD_CHECK(sa.volume == sb.volume && sa.area == sb.area);
        // Ida e volta de parâmetros reproduz a mesma geometria.
        CadDocument c = CadDocument::withTestPart("c", p);
        CAD_CHECK(c.setParameters({200.0, 100.0, 10.0, 5.0}).ok);
        CAD_CHECK(c.setParameters(p).ok);
        const ShapeSummary sc = ShapeInspector::inspect(c.shape());
        CAD_CHECK(sc.volume == sa.volume && sc.area == sa.area && sc.faces == sa.faces);
        // Mesmos parâmetros: sem recálculo e sem marcar modificação.
        CadDocument d = CadDocument::withTestPart("d", p);
        const int rev = d.revision();
        CAD_CHECK(d.setParameters(p).ok);
        CAD_CHECK(d.revision() == rev && !d.isModified());
    }

    // 10-11. Rejeição de parâmetros inválidos e preservação do último estado válido
    std::printf("10/11. Parametros invalidos\n");
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        CadDocument v = CadDocument::withTestPart();
        checkRejected(v, {0.0, 60.0, 20.0, 10.0}, "length");
        checkRejected(v, {-5.0, 60.0, 20.0, 10.0}, "length");
        checkRejected(v, {100.0, 0.05, 20.0, 10.0}, "width");
        checkRejected(v, {100.0, 60.0, 0.0, 10.0}, "height");
        checkRejected(v, {100.0, 60.0, 20001.0, 10.0}, "height");
        checkRejected(v, {nan, 60.0, 20.0, 10.0}, "length");
        checkRejected(v, {100.0, inf, 20.0, 10.0}, "width");
        checkRejected(v, {100.0, 60.0, 20.0, 0.0}, "holeRadius");
        checkRejected(v, {100.0, 60.0, 20.0, -1.0}, "holeRadius");
        checkRejected(v, {100.0, 60.0, 20.0, 29.5}, "holeRadius"); // parede < 1 mm em Y
        checkRejected(v, {100.0, 60.0, 20.0, 30.0}, "holeRadius"); // furo tangente
        checkRejected(v, {100.0, 60.0, 20.0, 45.0}, "holeRadius"); // maior que a largura
        checkRejected(v, {2.0, 60.0, 20.0, 0.5}, "holeRadius");    // bloco estreito demais
        // Limite exato aceito: raio = min(L,W)/2 - parede.
        CAD_CHECK(v.setParameters({100.0, 60.0, 20.0, 29.0}).ok);
        checkPart("limite", v.shape(), {100.0, 60.0, 20.0, 29.0});
        // Depois de recusas, ainda aceita valores válidos.
        CAD_CHECK(v.setParameters(PartParameters()).ok);
        cad::test::checkTestPart(ShapeInspector::inspect(v.shape()));
        CAD_CHECK(CadDocument::validate(PartParameters()).ok);
    }

    // Construção a partir de dados persistidos.
    std::printf("Construcao a partir de dados\n");
    {
        CadDocument out("original");
        std::string error;
        CAD_CHECK(CadDocument::fromData("lido", PartParameters{80.0, 40.0, 10.0, 5.0}, "bloco-A", "furo_B", out, error));
        CAD_CHECK(out.name() == "lido" && !out.isModified());
        CAD_CHECK(out.operations().at(0).id == "bloco-A" && out.operations().at(1).id == "furo_B");
        checkPart("fromData", out.shape(), {80.0, 40.0, 10.0, 5.0});

        CadDocument keep("mantido");
        CAD_CHECK(!CadDocument::fromData("x", PartParameters{80.0, 40.0, 10.0, 50.0}, "a", "b", keep, error));
        CAD_CHECK(!CadDocument::fromData("x", PartParameters(), "a", "a", keep, error));
        CAD_CHECK(!CadDocument::fromData("x", PartParameters(), "id com espaço", "b", keep, error));
        CAD_CHECK(keep.name() == "mantido");
    }

    // Nome e estado de modificação.
    {
        CadDocument n;
        n.setName("Flange");
        CAD_CHECK(n.name() == "Flange" && n.isModified());
        n.setModified(false);
        CAD_CHECK(!n.isModified());
    }

    return cad::test::finish("test_documento");
}
