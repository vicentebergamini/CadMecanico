// Teste geométrico da peça de teste (sem Qt, sem arquivos).
#include "ExpectedTestPart.h"

#include "modeling/ShapeInspector.h"
#include "modeling/TestPart.h"

#include <TopoDS_Shape.hxx>

using cad::modeling::ShapeInspector;

int main()
{
    const TopoDS_Shape peca = cad::modeling::buildTestPart();
    const auto resumo = ShapeInspector::inspect(peca);

    std::printf("Peca de teste (modelagem):\n");
    cad::test::printSummary(resumo);
    cad::test::checkTestPart(resumo);

    // Topologia completa da peça construída diretamente.
    CAD_CHECK(resumo.shells == 1);
    CAD_CHECK(resumo.edges == 15);
    CAD_CHECK(resumo.vertices == 10);

    // Shape nulo não deve quebrar o inspetor.
    const auto nulo = ShapeInspector::inspect(TopoDS_Shape());
    CAD_CHECK(nulo.isNull);
    CAD_CHECK(!nulo.isValid);

    return cad::test::finish("test_geometria");
}
