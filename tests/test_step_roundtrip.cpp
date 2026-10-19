// Exporta a peça de teste em STEP, relê o arquivo e compara com o original.
// Uso: test_step_roundtrip [diretorio_de_saida]
#include "ExpectedTestPart.h"

#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "modeling/TestPart.h"

#include <filesystem>

namespace fs = std::filesystem;
using cad::modeling::ShapeInspector;

int main(int argc, char* argv[])
{
    const fs::path dir = argc > 1 ? fs::u8path(argv[1]) : fs::temp_directory_path() / "cadmecanico_tests";
    fs::create_directories(dir);
    const fs::path arquivo = dir / "teste_peca.step";
    std::error_code ec;
    fs::remove(arquivo, ec);

    const TopoDS_Shape original = cad::modeling::buildTestPart();

    const auto escrita = cad::io::StepExporter().write(original, arquivo);
    std::printf("Exportacao: %s %s\n", escrita.ok ? "OK" : "FALHOU", escrita.message.c_str());
    CAD_CHECK(escrita.ok);
    CAD_CHECK(fs::exists(arquivo));
    if (fs::exists(arquivo))
        std::printf("  Arquivo: %s (%llu bytes)\n", arquivo.u8string().c_str(),
                    static_cast<unsigned long long>(fs::file_size(arquivo)));

    const auto leitura = cad::io::StepImporter().read(arquivo);
    std::printf("Leitura: %s %s\n", leitura.ok ? "OK" : "FALHOU", leitura.message.c_str());
    std::printf("  Raizes: %d | Transferidas: %d\n", leitura.rootCount, leitura.transferredCount);
    CAD_CHECK(leitura.ok);
    CAD_CHECK(leitura.rootCount == 1);
    CAD_CHECK(leitura.transferredCount == 1);

    const auto resumo = ShapeInspector::inspect(leitura.shape);
    std::printf("Peca relida do STEP:\n");
    cad::test::printSummary(resumo);
    cad::test::checkTestPart(resumo);

    // Erros devem ser reportados sem exceção.
    CAD_CHECK(!cad::io::StepImporter().read(dir / "inexistente.step").ok);
    CAD_CHECK(!cad::io::StepExporter().write(TopoDS_Shape(), dir / "nulo.step").ok);

    return cad::test::finish("test_step_roundtrip");
}
