#include "io/StepImporter.h"

#include <STEPControl_Reader.hxx>

namespace cad::io {

StepReadResult StepImporter::read(const std::filesystem::path& file) const
{
    StepReadResult result;

    // O OCCT espera caminhos em UTF-8.
    const std::string path = file.u8string();
    STEPControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone)
    {
        result.message = "Falha ao ler " + path;
        return result;
    }

    result.rootCount        = reader.NbRootsForTransfer();
    result.transferredCount = reader.TransferRoots();
    if (result.transferredCount == 0)
    {
        result.message = "Nenhuma raiz transferida de " + path;
        return result;
    }

    result.shape = reader.OneShape();
    result.ok    = !result.shape.IsNull();
    if (!result.ok)
        result.message = "Shape resultante nulo";
    return result;
}

} // namespace cad::io
