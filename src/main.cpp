// Ponto de entrada do CAD Mecânico.
#include <QApplication>
#include <QScreen>

#include "app/MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("CadMecanico");
    QApplication::setApplicationVersion(QStringLiteral(CADMECANICO_VERSION));

    cad::app::MainWindow window;
    // Tamanho inicial limitado à área útil da tela (telas pequenas ou com escala alta).
    const QSize available = window.screen()->availableGeometry().size();
    window.resize(QSize(1280, 800).boundedTo(available * 0.9));
    window.show();
    return app.exec();
}
