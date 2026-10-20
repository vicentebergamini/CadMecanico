// Integração da janela principal com o visualizador 3D, com contexto OpenGL real.
#include "ViewerTestUtils.h"

#include "app/MainWindow.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QDockWidget>
#include <QLocale>
#include <QScreen>
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
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
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
    // E4
    void barraDeStatusSelecao();
    void teclaFNaJanela();
    void exportarStepAposInteracoes();
    void redimensionarAposInteracoes();
    // E5.1
    void visualizadorAtualizadoAposEdicao();
    void navegacaoESelecaoAposRecalculo();
    void selecaoNaArvoreIndependenteDa3D();
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

void TestAppViewer::barraDeStatusSelecao()
{
    OcctViewerWidget* v = viewer();
    auto at = [v](const gp_Pnt& p) { return toLogical(v, project(v, p)); };
    auto message = [this] { return m_window->statusBar()->currentMessage(); };

    QVERIFY(click(v, at(gp_Pnt(25.0, 15.0, 20.0))));
    QTRY_VERIFY(message().startsWith("Face"));
    qInfo("%s", qPrintable(message()));
    QVERIFY(message().contains("/7"));
    QVERIFY(message().contains("Plana"));
    QVERIFY(message().contains(QString::fromUtf8("Área: 5.685,84 mm²")));
    saveEvidence(m_window->grab().toImage(), "E4_10_status_face");
    pauseBetweenClicks();

    QVERIFY(click(v, at(gp_Pnt(50.0, 0.0, 20.0))));
    QTRY_VERIFY(message().startsWith("Aresta"));
    qInfo("%s", qPrintable(message()));
    QVERIFY(message().contains("/15"));
    QVERIFY(message().contains("Reta"));
    QVERIFY(message().contains("Comprimento: 100,00 mm"));
    saveEvidence(m_window->grab().toImage(), "E4_11_status_aresta");
    pauseBetweenClicks();

    QVERIFY(click(v, at(gp_Pnt(50.0, 20.0, 20.0))));
    QTRY_VERIFY(message().contains("Circular"));
    qInfo("%s", qPrintable(message()));
    QVERIFY(message().contains("(R 10,00 mm)"));
    QVERIFY(message().contains("Comprimento: 62,83 mm"));
    pauseBetweenClicks();

    QVERIFY(click(v, at(gp_Pnt(100.0, 0.0, 20.0))));
    QTRY_VERIFY(message().startsWith(QString::fromUtf8("Vértice")));
    qInfo("%s", qPrintable(message()));
    QVERIFY(message().contains("/10"));
    QVERIFY(message().contains("X 100,00  Y 0,00  Z 20,00 mm"));
    saveEvidence(m_window->grab().toImage(), "E4_12_status_vertice");
    pauseBetweenClicks();

    QVERIFY(click(v, QPointF(12.0, v->height() / 2.0)));
    QTRY_VERIFY(message().contains("Open CASCADE"));
}

void TestAppViewer::teclaFNaJanela()
{
    const auto& cam = viewer()->view()->Camera();
    cam->SetScale(cam->Scale() * 6.0);
    viewer()->view()->Invalidate();
    viewer()->setFocus();
    QTest::keyClick(viewer(), Qt::Key_F);
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    const DiffResult d = diff(img, backgroundReference(viewer()));
    QVERIFY(d.box.width() > 0.8 * img.width() || d.box.height() > 0.8 * img.height());
}

void TestAppViewer::exportarStepAposInteracoes()
{
    OcctViewerWidget* v = viewer();
    const QPointF c(v->width() / 2.0, v->height() / 2.0);
    QVERIFY(drag(v, c, c + QPointF(80, 40), Qt::MiddleButton));
    QVERIFY(drag(v, c, c + QPointF(-50, 30), Qt::MiddleButton, Qt::ControlModifier));
    sendWheel(v, c, 120);
    v->fitAll();
    QVERIFY(waitFrames(v));
    QVERIFY(click(v, toLogical(v, project(v, gp_Pnt(25.0, 15.0, 20.0)))));
    QTRY_COMPARE(v->selectedShapes().size(), 1);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    m_window->nextPath = dir.filePath("apos_interacoes.step");
    action("actionExportarStep")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(m_window->errors.isEmpty());

    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = cad::modeling::ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 7);
    QVERIFY(qAbs(s.volume - kExpectedVolume) <= kExpectedVolume * 1e-9);
    // Geometria em memória intacta e seleção preservada.
    QVERIFY(v->shape().IsSame(m_window->part()));
    QCOMPARE(v->selectedShapes().size(), 1);
}

void TestAppViewer::redimensionarAposInteracoes()
{
    OcctViewerWidget* v = viewer();
    const QPointF c(v->width() / 2.0, v->height() / 2.0);
    QVERIFY(drag(v, c, c + QPointF(70, -30), Qt::MiddleButton));
    const QSize available = m_window->screen()->availableGeometry().size() * 0.85;
    for (QSize s : {QSize(800, 560), QSize(1150, 760)})
    {
        s = s.boundedTo(available);
        m_window->resize(s);
        QTRY_COMPARE(m_window->size(), s);
        QVERIFY(waitFrames(v));
        const QImage img = v->grabViewerImage();
        int w = 0, h = 0;
        v->view()->Window()->Size(w, h);
        QCOMPARE(QSize(w, h), img.size());
        v->fitAll();
        const DiffResult d = diff(v->grabViewerImage(), backgroundReference(v));
        QVERIFY(d.count > 0);
    }
    saveEvidence(m_window->grab().toImage(), "E4_13_janela_redimensionada");
}

void TestAppViewer::visualizadorAtualizadoAposEdicao()
{
    action("actionVistaSuperior")->trigger();
    const QImage before = viewer()->grabViewerImage();
    const DiffResult d0 = diff(before, backgroundReference(viewer()));
    qInfo("Antes: proporcao %.3f", double(d0.box.width()) / d0.box.height());
    QVERIFY(std::abs(double(d0.box.width()) / d0.box.height() - 100.0 / 60.0) < 0.05 * 100.0 / 60.0);

    const cad::document::PartParameters p{150.0, 50.0, 20.0, 12.0};
    QVERIFY(m_window->applyParameters(p));
    QVERIFY(viewer()->shape().IsSame(m_window->part()));
    QVERIFY(waitFrames(viewer()));
    // Peça recalculada aparece enquadrada e com a nova proporção na vista superior.
    const QImage after = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), "E51_01_vista_superior_apos_edicao");
    const DiffResult d1 = diff(after, backgroundReference(viewer()));
    const double aspect = double(d1.box.width()) / d1.box.height();
    qInfo("Depois: proporcao %.3f (esperada %.3f)", aspect, 150.0 / 50.0);
    QVERIFY(std::abs(aspect - 3.0) < 0.05 * 3.0);
    QVERIFY(d1.box.width() > 0.8 * after.width() || d1.box.height() > 0.8 * after.height());
    QVERIFY(viewer()->view()->Camera()->IsOrthographic());
}

void TestAppViewer::navegacaoESelecaoAposRecalculo()
{
    const cad::document::PartParameters p{150.0, 80.0, 30.0, 15.0};
    QVERIFY(m_window->applyParameters(p));
    QVERIFY(waitFrames(viewer()));
    OcctViewerWidget* v = viewer();
    auto at = [v](const gp_Pnt& pt) { return toLogical(v, project(v, pt)); };

    // Seleção de face da nova geometria: área calculada com os novos parâmetros.
    QVERIFY(click(v, at(gp_Pnt(30.0, 20.0, 30.0))));
    QTRY_VERIFY(m_window->statusBar()->currentMessage().startsWith("Face"));
    const double area = 150.0 * 80.0 - 3.14159265358979323846 * 15.0 * 15.0;
    const QString expected = QLocale(QLocale::Portuguese, QLocale::Brazil).toString(area, 'f', 2);
    qInfo("%s", qPrintable(m_window->statusBar()->currentMessage()));
    QVERIFY(m_window->statusBar()->currentMessage().contains(expected));
    saveEvidence(m_window->grab().toImage(), "E51_02_selecao_apos_recalculo");
    pauseBetweenClicks();

    // Aresta do furo com o novo raio.
    QVERIFY(click(v, at(gp_Pnt(75.0, 40.0 - 15.0, 30.0))));
    QTRY_VERIFY(m_window->statusBar()->currentMessage().contains("(R 15,00 mm)"));
    pauseBetweenClicks();

    // Navegação continua funcionando.
    const gp_Dir before = v->view()->Camera()->Direction();
    const QPointF c(v->width() / 2.0, v->height() / 2.0);
    QVERIFY(drag(v, c, c + QPointF(100, 40), Qt::MiddleButton));
    QVERIFY(before.Angle(v->view()->Camera()->Direction()) > 0.1);
    sendWheel(v, c, 120);
    QVERIFY(waitFrames(v));
    QTest::keyClick(v, Qt::Key_F);
    QVERIFY(waitFrames(v));
    QCOMPARE(v->selectedShapes().size(), 1);
}

void TestAppViewer::selecaoNaArvoreIndependenteDa3D()
{
    OcctViewerWidget* v = viewer();
    QVERIFY(click(v, toLogical(v, project(v, gp_Pnt(25.0, 15.0, 20.0)))));
    QTRY_COMPARE(v->selectedShapes().size(), 1);
    const TopoDS_Shape face = v->selectedShapes().first();

    QTreeWidget* tree = m_window->operationsTree();
    QTreeWidgetItem* hole = tree->topLevelItem(0)->child(1);
    tree->setCurrentItem(hole);
    QCOMPARE(hole->data(0, cad::app::MainWindow::kItemIdRole).toString(), QStringLiteral("op-2"));
    // A seleção de operação não altera a seleção geométrica (e vice-versa).
    QCOMPARE(v->selectedShapes().size(), 1);
    QVERIFY(v->selectedShapes().first().IsSame(face));
    pauseBetweenClicks();
    QVERIFY(click(v, QPointF(8.0, 8.0))); // canto superior esquerdo: fundo
    QTRY_VERIFY(v->selectedShapes().isEmpty());
    QCOMPARE(tree->currentItem(), hole);
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
