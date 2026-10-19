// Teste de integração: Qt 6 + OCCT (modelagem, booleana e exportação STEP).
#include <QApplication>
#include <QLabel>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Version.hxx>
#include <gp_Ax2.hxx>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    TopoDS_Shape bloco = BRepPrimAPI_MakeBox(100.0, 60.0, 20.0).Shape();
    TopoDS_Shape furo  = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 30, -1), gp::DZ()), 10.0, 22.0).Shape();
    TopoDS_Shape peca  = BRepAlgoAPI_Cut(bloco, furo).Shape();

    STEPControl_Writer writer;
    writer.Transfer(peca, STEPControl_AsIs);
    const bool ok = writer.Write("teste_peca.step") == IFSelect_RetDone;

    QLabel label(QString("Qt %1 + OCCT %2\nExportacao STEP: %3")
                     .arg(qVersion(), OCC_VERSION_COMPLETE, ok ? "OK" : "FALHOU"));
    label.setMinimumSize(320, 80);
    label.setAlignment(Qt::AlignCenter);
    label.show();
    return app.exec();
}
