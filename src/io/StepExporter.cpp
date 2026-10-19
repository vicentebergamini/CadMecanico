#include "io/StepExporter.h"

#include <STEPControl_Writer.hxx>

namespace cad::io {

StepWriteResult StepExporter::write(const TopoDS_Shape& shape, const std::filesystem::path& file) const
{
    StepWriteResult result;
    if (shape.IsNull())
    {
        result.message = "Shape nulo";
        return result;
    }

    STEPControl_Writer writer;
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone)
    {
        result.message = "Falha na transferencia do shape para o modelo STEP";
        return result;
    }

    // O OCCT espera caminhos em UTF-8.
    const std::string path = file.u8string();
    if (writer.Write(path.c_str()) != IFSelect_RetDone)
    {
        result.message = "Falha ao gravar " + path;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace cad::io
