// Integração da janela principal com o visualizador 3D, com contexto OpenGL real.
#include "ViewerTestUtils.h"

#include "app/MainWindow.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeWidget>

#include <Graphic3d_Camera.hxx>
#include <gp_Dir.hxx>

#include <filesystem>

using cad::viewer::OcctViewerWidget;
using namespace cad::test::viewer;

namespace {

constexpr double kExpectedVolume = 100.0 * 60.0 * 20.0 - 3.14159265358979323846 * 10.0 * 10.0 * 20.0;

class TestableMainWindow : public cad::app::MainWindow
{
public:
    QString nextPath;
    QStringList infos;
    QStringList errors;

protected:
    QString askExportPath(const QString&) override { return nextPath; }
    bool confirmOverwrite(const QString&) override { return true; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
    void reportError(const QString&, const QString& text) override { errors << text; }
};

} // namespace

class TestAppViewer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void visualizadorNaAreaCentral();
    void renderizaPecaNaJanela();
    void acoesDeVistaPeloMenu();
    void ajustarPelaAcao();
    void recursosE2Preservados();
    void exportarStepAposVistas();
    void fechamentoNormal();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }

    TestableMainWindow* m_window = nullptr;
};

void TestAppViewer::init()
{
    m_window = new TestableMainWindow;
    m_window->resize(1100, 750);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    QVERIFY(viewer());
    QVERIFY2(waitViewerReady(viewer()), "visualizador não inicializado na janela principal");
}

void TestAppViewer::cleanup()
{
    delete m_window;
    m_window = nullptr;
}

void TestAppViewer::visualizadorNaAreaCentral()
{
    QCOMPARE(m_window->centralWidget(), static_cast<QWidget*>(viewer()));
    QCOMPARE(viewer()->objectName(), QStringLiteral("visualizador3D"));
    QVERIFY(viewer()->isVisible());
    QVERIFY(viewer()->width() >= 400 && viewer()->height() >= 300);
    QVERIFY(viewer()->shape().IsSame(m_window->part()));
}

void TestAppViewer::renderizaPecaNaJanela()
{
    const QImage img = viewer()->grabViewerImage();
    const DiffResult d = diff(img, backgroundReference(viewer()));
    QVERIFY(double(d.count) / (img.width() * img.height()) > 0.15);
    saveEvidence(m_window->grab().toImage(), "E3_10_janela_principal");
}

void TestAppViewer::acoesDeVistaPeloMenu()
{
    auto* views = m_window->findChild<QMenu*>("menuVistasPadrao");
    QVERIFY(views && views->isEnabled());
    struct Case { const char* action; gp_Dir dir; } cases[] = {
        {"actionVistaFrente", gp_Dir(0, 1, 0)},
        {"actionVistaSuperior", gp_Dir(0, 0, -1)},
        {"actionVistaDireita", gp_Dir(-1, 0, 0)},
        {"actionVistaIsometrica", gp_Dir(-1, 1, -1)},
    };
    for (const Case& c : cases)
    {
        QAction* a = action(c.action);
        QVERIFY(a && a->isEnabled());
        a->trigger();
        QVERIFY2(viewer()->view()->Camera()->Direction().Angle(c.dir) < 1e-3, c.action);
        QVERIFY(waitFrames(viewer()));
    }
}

void TestAppViewer::ajustarPelaAcao()
{
    const auto& cam = viewer()->view()->Camera();
    cam->SetScale(cam->Scale() * 6.0);
    viewer()->view()->Invalidate();

    QAction* fit = action("actionAjustarJanela");
    QVERIFY(fit && fit->isEnabled());
    QCOMPARE(fit->shortcut(), QKeySequence(Qt::Key_F));
    fit->trigger();

    const QImage img = viewer()->grabViewerImage();
    const DiffResult d = diff(img, backgroundReference(viewer()));
    QVERIFY(d.box.width() > 0.8 * img.width() || d.box.height() > 0.8 * img.height());
}

void TestAppViewer::recursosE2Preservados()
{
    for (const char* menu : {"menuArquivo", "menuEditar", "menuVisualizar", "menuAjuda"})
        QVERIFY2(m_window->findChild<QMenu*>(menu), menu);
    for (const char* bar : {"toolBarArquivo", "toolBarModelagem", "toolBarVisualizar"})
        QVERIFY2(m_window->findChild<QToolBar*>(bar), bar);
    auto* dock = m_window->findChild<QDockWidget*>("dockArvoreOperacoes");
    QVERIFY(dock && dock->isVisible());
    QCOMPARE(m_window->findChild<QTreeWidget*>("arvoreOperacoes")->topLevelItemCount(), 1);
    QVERIFY(m_window->statusBar()->currentMessage().contains("Open CASCADE"));
}

void TestAppViewer::exportarStepAposVistas()
{
    action("actionVistaFrente")->trigger();
    action("actionVistaIsometrica")->trigger();
    action("actionAjustarJanela")->trigger();
    QVERIFY(waitFrames(viewer()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    m_window->nextPath = dir.filePath("apos_vistas.step");
    action("actionExportarStep")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(m_window->errors.isEmpty());

    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = cad::modeling::ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.faces, 7);
    QVERIFY(qAbs(s.volume - kExpectedVolume) <= kExpectedVolume * 1e-9);
}

void TestAppViewer::fechamentoNormal()
{
    action("actionSair")->trigger();
    QTRY_VERIFY(!m_window->isVisible());
    delete m_window; // libera o visualizador e o contexto OpenGL
    m_window = nullptr;
}

QTEST_MAIN(TestAppViewer)
#include "test_app_viewer.moc"
