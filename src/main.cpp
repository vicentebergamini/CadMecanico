// Teste de integração: Qt 6 + OCCT (modelagem, booleana e exportação STEP).
#include <QApplication>
#include <QLabel>

#include <Standard_Version.hxx>

#include "io/StepExporter.h"
#include "modeling/TestPart.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    const TopoDS_Shape peca = cad::modeling::buildTestPart();

    // Gravado no diretório de trabalho, como na versão inicial.
    const bool ok = cad::io::StepExporter().write(peca, "teste_peca.step").ok;

    QLabel label(QString("Qt %1 + OCCT %2\nExportacao STEP: %3")
                     .arg(qVersion(), OCC_VERSION_COMPLETE, ok ? "OK" : "FALHOU"));
    label.setMinimumSize(320, 80);
    label.setAlignment(Qt::AlignCenter);
    label.show();
    return app.exec();
}
