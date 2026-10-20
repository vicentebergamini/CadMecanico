// Esboços 2D na interface real (E5.2), com contexto OpenGL real: modo de esboço, câmera
// perpendicular, criação e seleção pelo mouse, dimensões, árvore, persistência e preservação do
// sólido, da navegação 3D, do ViewCube e da exportação STEP.
#include "ExpectedTestPart.h"
#include "ViewerTestUtils.h"

#include "app/MainWindow.h"
#include "app/SketchEntityDialog.h"
#include "document/DocumentFile.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeWidget>

#include <AIS_ViewCube.hxx>
#include <Graphic3d_Camera.hxx>
#include <gp_Dir.hxx>

#include <cmath>
#include <filesystem>
#include <optional>

using cad::app::MainWindow;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kExpectedVolume = 100.0 * 60.0 * 20.0 - 3.14159265358979323846 * 10.0 * 10.0 * 20.0;

class TestableMainWindow : public MainWindow
{
public:
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    std::optional<SketchGeometry> nextGeometry;
    QString nextSavePath;
    QString nextOpenPath;
    QString nextExportPath;
    QStringList errors;
    QStringList infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    std::optional<SketchGeometry> askEntityDimensions(const SketchEntity&) override { return nextGeometry; }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    QString askExportPath(const QString&) override { return nextExportPath; }
    bool confirmOverwrite(const QString&) override { return true; }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
};

bool approx(double a, double b, double tol = 0.35)
{
    return std::fabs(a - b) <= tol;
}

QColor colorAt(const QImage& img, QPoint p)
{
    return QColor(meanAround(img, p, 1));
}

// Pixel azul das entidades do esboço (ou laranja se selecionada) em uma vizinhança.
bool hasSketchColorNear(const QImage& img, QPoint c, int r, bool selected)
{
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
        {
            const QRgb p = pixelAt(img, c + QPoint(dx, dy));
            if (selected ? (qRed(p) > 170 && qRed(p) - qBlue(p) > 90)
                         : (qBlue(p) > 140 && qBlue(p) - qRed(p) > 80 && qGreen(p) < 140))
                return true;
        }
    return false;
}

QTreeWidgetItem* findItem(QTreeWidget* tree, const QString& kind, const QString& id, const QString& entity = {})
{
    QTreeWidgetItem* root = tree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
    {
        QTreeWidgetItem* item = root->child(i);
        if (item->data(0, MainWindow::kItemIdRole).toString() != id)
            continue;
        if (kind == "esboco" && item->data(0, MainWindow::kItemKindRole).toString() == kind)
            return item;
        for (int k = 0; k < item->childCount(); ++k)
            if (item->child(k)->data(0, MainWindow::kItemEntityRole).toString() == entity)
                return item->child(k);
    }
    return nullptr;
}

} // namespace

class TestEsbocoUi : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void novoEsbocoNoPlano_data();
    void novoEsbocoNoPlano();
    void criacaoPeloMouse();
    void escCancelaPontoPendente();
    void exibicaoNoOpenGL();
    void selecaoDeEntidades2D();
    void navegacaoNoModoEsboco();
    void edicaoDeDimensoesPelaInterface();
    void dialogoDeDimensoesReal();
    void cancelarPreservaEstado();
    void concluirAtualizaArvoreEDocumento();
    void editarEsbocoExistentePelaArvore();
    void salvarEAbrirComEsbocos();
    void solidoEStepPreservados();
    void navegacaoESelecao3DForaDoEsboco();
    void viewCubeEVistasAposEsboco();
    void fechamentoDuranteEdicao();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        const SketchPlane& plane = m_window->editingSketch()->plane();
        return toLogical(viewer(), project(viewer(), plane.toGlobal(u, v)));
    }
    // Cria linha, retângulo e círculo com o mouse no esboço em edição.
    void drawThreeEntities();
    void tool(const char* name) { action(name)->trigger(); }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestEsbocoUi::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
    m_window = new TestableMainWindow;
    const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size() * 0.85;
    m_window->resize(QSize(1200, 800).boundedTo(available));
    m_window->move(20, 20);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    // A peça de teste não é mais criada na inicialização: criada explicitamente (exemplo).
    m_window->findChild<QAction*>("actionExemploBlocoFuro")->trigger();
    QVERIFY(m_window->document().hasPart());
    QVERIFY(waitViewerReady(viewer()));

    pauseBetweenClicks();
}

void TestEsbocoUi::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestEsbocoUi::drawThreeEntities()
{
    tool("actionFerramentaLinha");
    QVERIFY(click(viewer(), at(10.0, 10.0)));
    pauseBetweenClicks();
    sendMouse(viewer(), QEvent::MouseMove, at(40.0, 10.0), Qt::NoButton, Qt::NoButton); // pré-visualização
    QVERIFY(waitFrames(viewer()));
    QVERIFY(click(viewer(), at(60.0, 10.0)));
    pauseBetweenClicks();

    tool("actionFerramentaRetangulo");
    QVERIFY(click(viewer(), at(50.0, 40.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(20.0, 20.0))); // cantos em qualquer ordem
    pauseBetweenClicks();

    tool("actionFerramentaCirculo");
    QVERIFY(click(viewer(), at(75.0, 30.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(85.0, 30.0)));
    pauseBetweenClicks();
    tool("actionFerramentaSelecionar");
}

void TestEsbocoUi::novoEsbocoNoPlano_data()
{
    QTest::addColumn<int>("plane");
    QTest::addColumn<double>("dx");
    QTest::addColumn<double>("dy");
    QTest::addColumn<double>("dz");
    QTest::newRow("XY") << int(PlaneType::XY) << 0.0 << 0.0 << -1.0;
    QTest::newRow("XZ") << int(PlaneType::XZ) << 0.0 << 1.0 << 0.0;
    QTest::newRow("YZ") << int(PlaneType::YZ) << -1.0 << 0.0 << 0.0;
}

void TestEsbocoUi::novoEsbocoNoPlano()
{
    QFETCH(int, plane);
    QFETCH(double, dx);
    QFETCH(double, dy);
    QFETCH(double, dz);

    m_window->nextPlane = PlaneType(plane);
    action("actionNovoEsboco")->trigger();
    QVERIFY(m_window->isEditingSketch());
    QVERIFY(viewer()->isSketchMode());
    const Sketch* s = m_window->editingSketch();
    QCOMPARE(s->id(), std::string("sk-1"));
    QVERIFY(s->plane().type() == PlaneType(plane));
    // 18. Câmera perpendicular ao plano, ortográfica, eixo vertical local para cima.
    const auto& cam = viewer()->view()->Camera();
    QVERIFY(cam->Direction().IsEqual(gp_Dir(dx, dy, dz), 1e-6));
    QVERIFY(cam->Direction().IsOpposite(s->plane().normal(), 1e-6));
    QVERIFY(cam->OrthogonalizedUp().IsEqual(s->plane().verticalAxis(), 1e-6));
    QVERIFY(cam->IsOrthographic());
    // Eixo horizontal local aponta para a direita na tela (sem espelhamento).
    const QPoint o = project(viewer(), s->plane().toGlobal(0, 0));
    const QPoint h = project(viewer(), s->plane().toGlobal(20, 0));
    const QPoint v = project(viewer(), s->plane().toGlobal(0, 20));
    QVERIFY(h.x() > o.x() + 10 && std::abs(h.y() - o.y()) <= 1);
    QVERIFY(v.y() < o.y() - 10 && std::abs(v.x() - o.x()) <= 1);
    // ViewCube, sólido e eixos globais ocultos e vistas padrão desabilitadas no modo de
    // esboço (E5.2.1); documento ainda intacto.
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->originAxes()));
    QVERIFY(m_window->part().IsSame(viewer()->shape()));
    QVERIFY(!action("actionVistaFrente")->isEnabled());
    QVERIFY(action("actionFerramentaLinha")->isEnabled());
    QVERIFY(m_window->document().sketches().empty());
    QTreeWidgetItem* item = findItem(m_window->operationsTree(), "esboco", "sk-1");
    QVERIFY(item);
    QVERIFY(item->text(0).contains(QStringLiteral("em edição")));
    QVERIFY(item->text(0).contains(QString::fromLatin1(SketchPlane::name(PlaneType(plane)))));
    // Eixos locais visíveis: H (vermelho) e V (verde) a partir da origem.
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), QStringLiteral("E52_01_modo_esboco_%1").arg(QTest::currentDataTag()));
    const QPoint hMid = project(viewer(), s->plane().toGlobal(15, 0));
    const QPoint vMid = project(viewer(), s->plane().toGlobal(0, 15));
    auto hasColor = [&img](QPoint c, auto pred) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx2 = -2; dx2 <= 2; ++dx2)
                if (pred(QColor(pixelAt(img, c + QPoint(dx2, dy)))))
                    return true;
        return false;
    };
    QVERIFY(hasColor(hMid, [](QColor c) { return c.red() > 150 && c.green() < 90 && c.blue() < 90; }));
    QVERIFY(hasColor(vMid, [](QColor c) { return c.green() > 100 && c.red() < 90 && c.blue() < 90; }));
}

void TestEsbocoUi::criacaoPeloMouse()
{
    m_window->nextPlane = PlaneType::XY;
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();

    const Sketch* s = m_window->editingSketch();
    QCOMPARE(s->entities().size(), size_t(3));
    const auto& line = std::get<SketchLine>(s->findEntity("e-1")->geometry);
    qInfo("Linha: (%.2f, %.2f) -> (%.2f, %.2f)", line.x1, line.y1, line.x2, line.y2);
    QVERIFY(approx(line.x1, 10.0) && approx(line.y1, 10.0) && approx(line.x2, 60.0) && approx(line.y2, 10.0));
    const auto& rect = std::get<SketchRectangle>(s->findEntity("e-2")->geometry);
    qInfo("Retangulo: (%.2f, %.2f) %.2f x %.2f", rect.x, rect.y, rect.width, rect.height);
    QVERIFY(approx(rect.x, 20.0) && approx(rect.y, 20.0) && approx(rect.width, 30.0) && approx(rect.height, 20.0));
    const auto& circle = std::get<SketchCircle>(s->findEntity("e-3")->geometry);
    qInfo("Circulo: (%.2f, %.2f) R %.2f", circle.cx, circle.cy, circle.radius);
    QVERIFY(approx(circle.cx, 75.0) && approx(circle.cy, 30.0) && approx(circle.radius, 10.0));
    // Coordenadas e medidas derivadas arredondadas a 0,1 mm, sem resíduo de ponto flutuante.
    for (double v : {line.x1, line.y1, line.x2, line.y2, rect.x, rect.y, rect.width, rect.height, circle.cx,
                     circle.cy, circle.radius})
        QCOMPARE(v, std::round(v * 10.0) / 10.0);
    // A última criada fica selecionada.
    QCOMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-3"));
    QVERIFY(findItem(m_window->operationsTree(), "entidade", "sk-1", "e-3")->text(0).startsWith(QStringLiteral("Círculo e-3")));
    QCOMPARE(m_window->operationsTree()->currentItem()->data(0, MainWindow::kItemEntityRole).toString(),
             QStringLiteral("e-3"));
    saveEvidence(m_window->grab().toImage(), "E52_02_criacao_pelo_mouse");
}

void TestEsbocoUi::escCancelaPontoPendente()
{
    action("actionNovoEsboco")->trigger();
    tool("actionFerramentaLinha");
    QVERIFY(click(viewer(), at(10.0, 10.0)));
    QTest::keyClick(viewer(), Qt::Key_Escape);
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(30.0, 30.0))); // novo primeiro ponto, não um segundo ponto
    QVERIFY(m_window->editingSketch()->entities().empty());
    // Linha de comprimento zero (dois cliques no mesmo ponto) é recusada sem travar.
    QVERIFY(click(viewer(), at(30.0, 30.0)));
    QVERIFY(m_window->editingSketch()->entities().empty());
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("não criada")));
}

void TestEsbocoUi::exibicaoNoOpenGL()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    m_window->selectSketchEntity(QString()); // nenhuma selecionada: todas azuis
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(img, "E52_03_entidades_opengl");
    const SketchPlane& p = m_window->editingSketch()->plane();
    const int r = int(std::lround(2 * viewer()->devicePixelRatioF()));
    QVERIFY2(hasSketchColorNear(img, project(viewer(), p.toGlobal(35.0, 10.0)), r, false), "linha não desenhada");
    QVERIFY2(hasSketchColorNear(img, project(viewer(), p.toGlobal(35.0, 40.0)), r, false), "retângulo não desenhado");
    QVERIFY2(hasSketchColorNear(img, project(viewer(), p.toGlobal(20.0, 30.0)), r, false), "lado do retângulo");
    QVERIFY2(hasSketchColorNear(img, project(viewer(), p.toGlobal(85.0, 30.0)), r, false), "círculo não desenhado");
    QVERIFY2(hasSketchColorNear(img, project(viewer(), p.toGlobal(75.0, 40.0)), r, false), "círculo (topo)");
    // Ponto fora das entidades permanece sem a cor do esboço.
    QVERIFY(!hasSketchColorNear(img, project(viewer(), p.toGlobal(35.0, 30.0)), 0, false));
    // Entidade selecionada em laranja.
    m_window->selectSketchEntity("e-1");
    QVERIFY(waitFrames(viewer()));
    QVERIFY(hasSketchColorNear(viewer()->grabViewerImage(), project(viewer(), p.toGlobal(35.0, 10.0)), r, true));
}

void TestEsbocoUi::selecaoDeEntidades2D()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    QVERIFY(click(viewer(), at(35.0, 10.0)));
    QTRY_COMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-1"));
    QCOMPARE(m_window->operationsTree()->currentItem()->data(0, MainWindow::kItemEntityRole).toString(),
             QStringLiteral("e-1"));
    QVERIFY(action("actionDimensoesEntidade")->isEnabled());
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(50.0, 30.0))); // lado direito do retângulo
    QTRY_COMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-2"));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(75.0, 20.0))); // circunferência
    QTRY_COMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-3"));
    saveEvidence(m_window->grab().toImage(), "E52_04_selecao_circulo");
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(35.0, 30.0))); // área vazia (interior do retângulo)
    QTRY_VERIFY(m_window->selectedSketchEntity().isEmpty());
    QVERIFY(!action("actionDimensoesEntidade")->isEnabled());
    // Sólido não é selecionável no modo de esboço.
    QVERIFY(viewer()->selectedShapes().isEmpty());
    // Seleção pela árvore.
    m_window->operationsTree()->setCurrentItem(findItem(m_window->operationsTree(), "entidade", "sk-1", "e-2"));
    QCOMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-2"));
}

void TestEsbocoUi::navegacaoNoModoEsboco()
{
    m_window->nextPlane = PlaneType::XZ;
    action("actionNovoEsboco")->trigger();
    const auto& cam = viewer()->view()->Camera();
    const gp_Dir dir = cam->Direction();
    const gp_Pnt center = cam->Center();
    const QPointF c(viewer()->width() / 2.0, viewer()->height() / 2.0);
    // Botão central: desloca, não gira.
    QVERIFY(drag(viewer(), c, c + QPointF(80, 30), Qt::MiddleButton));
    QVERIFY(cam->Direction().IsEqual(dir, 1e-9));
    QVERIFY(cam->Center().Distance(center) > 1.0);
    const double scale = cam->Scale();
    sendWheel(viewer(), c, 120);
    QVERIFY(waitFrames(viewer()));
    QVERIFY(cam->Scale() < scale);
    QTest::keyClick(viewer(), Qt::Key_F);
    QVERIFY(waitFrames(viewer()));
    QVERIFY(cam->Direction().IsEqual(dir, 1e-9));
    // Arrasto com o botão esquerdo não cria nem seleciona.
    tool("actionFerramentaLinha");
    QVERIFY(drag(viewer(), c, c + QPointF(60, 0), Qt::LeftButton));
    QVERIFY(m_window->editingSketch()->entities().empty());
}

void TestEsbocoUi::edicaoDeDimensoesPelaInterface()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    m_window->selectSketchEntity("e-2");

    // Retângulo: largura e altura.
    m_window->nextGeometry = SketchRectangle{20.0, 20.0, 45.5, 12.25};
    action("actionDimensoesEntidade")->trigger();
    auto rect = std::get<SketchRectangle>(m_window->editingSketch()->findEntity("e-2")->geometry);
    QCOMPARE(rect.width, 45.5);
    QCOMPARE(rect.height, 12.25);
    QVERIFY(findItem(m_window->operationsTree(), "entidade", "sk-1", "e-2")->text(0).contains("45,50 x 12,25"));
    // Representação atualizada: novo canto direito desenhado.
    QVERIFY(waitFrames(viewer()));
    const SketchPlane& p = m_window->editingSketch()->plane();
    QVERIFY(hasSketchColorNear(viewer()->grabViewerImage(), project(viewer(), p.toGlobal(65.5, 26.0)),
                               int(std::lround(2 * viewer()->devicePixelRatioF())), true));

    // Linha: coordenadas.
    m_window->selectSketchEntity("e-1");
    m_window->nextGeometry = SketchLine{0.0, 0.0, 30.0, 40.0};
    action("actionDimensoesEntidade")->trigger();
    QCOMPARE(std::get<SketchLine>(m_window->editingSketch()->findEntity("e-1")->geometry).length(), 50.0);

    // Círculo: centro e raio.
    m_window->selectSketchEntity("e-3");
    m_window->nextGeometry = SketchCircle{-15.0, 5.0, 12.5};
    action("actionDimensoesEntidade")->trigger();
    const auto circle = std::get<SketchCircle>(m_window->editingSketch()->findEntity("e-3")->geometry);
    QVERIFY(circle.cx == -15.0 && circle.cy == 5.0 && circle.radius == 12.5);

    // 13. Inválido: recusado com mensagem; entidade mantida.
    m_window->nextGeometry = SketchCircle{-15.0, 5.0, 0.0};
    action("actionDimensoesEntidade")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains("Raio"));
    QCOMPARE(std::get<SketchCircle>(m_window->editingSketch()->findEntity("e-3")->geometry).radius, 12.5);
    // Cancelar o diálogo não altera nada.
    m_window->nextGeometry.reset();
    action("actionDimensoesEntidade")->trigger();
    QCOMPARE(std::get<SketchCircle>(m_window->editingSketch()->findEntity("e-3")->geometry).radius, 12.5);
    saveEvidence(m_window->grab().toImage(), "E52_05_dimensoes_editadas");
}

void TestEsbocoUi::dialogoDeDimensoesReal()
{
    SketchEntity e;
    e.id = "e-7";
    e.geometry = SketchCircle{10.0, 20.0, 5.0};
    cad::app::SketchEntityDialog dialog(e);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* radius = dialog.findChild<QDoubleSpinBox*>("dim_radius");
    auto* cx = dialog.findChild<QDoubleSpinBox*>("dim_cx");
    auto* diameter = dialog.findChild<QLabel*>("dim_diameter");
    auto* buttons = dialog.findChild<QDialogButtonBox*>("botoesDimensoes");
    QVERIFY(radius && cx && diameter && buttons);
    QCOMPARE(radius->suffix(), QStringLiteral(" mm"));
    QCOMPARE(diameter->text(), QStringLiteral("10,00 mm"));
    radius->setValue(0.0);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QVERIFY(dialog.isVisible());
    QVERIFY(dialog.validationMessage().contains("Raio"));
    radius->setValue(7.5);
    cx->setValue(-3.0);
    QCOMPARE(diameter->text(), QStringLiteral("15,00 mm")); // valor calculado acompanha a edição
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto g = std::get<SketchCircle>(dialog.geometry());
    QVERIFY(g.cx == -3.0 && g.cy == 20.0 && g.radius == 7.5);
}

void TestEsbocoUi::cancelarPreservaEstado()
{
    // Novo esboço cancelado não entra no documento.
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    action("actionCancelarEsboco")->trigger();
    QVERIFY(!m_window->isEditingSketch());
    QVERIFY(!viewer()->isSketchMode());
    QVERIFY(m_window->document().sketches().empty());
    QVERIFY(!m_window->document().isModified());

    // Esboço existente: alterações descartadas ao cancelar.
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    QVERIFY(m_window->finishSketch());
    const Sketch saved = *m_window->document().findSketch("sk-1");
    QVERIFY(m_window->editSketch("sk-1"));
    m_window->selectSketchEntity("e-2");
    action("actionExcluirEntidade")->trigger();
    m_window->selectSketchEntity("e-1");
    m_window->nextGeometry = SketchLine{0, 0, 99, 0};
    action("actionDimensoesEntidade")->trigger();
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(2));
    action("actionCancelarEsboco")->trigger();
    const Sketch* after = m_window->document().findSketch("sk-1");
    QCOMPARE(after->entities().size(), saved.entities().size());
    QCOMPARE(std::get<SketchLine>(after->findEntity("e-1")->geometry).x2,
             std::get<SketchLine>(saved.findEntity("e-1")->geometry).x2);
    QVERIFY(after->findEntity("e-2") != nullptr);
}

void TestEsbocoUi::concluirAtualizaArvoreEDocumento()
{
    m_window->nextPlane = PlaneType::YZ;
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    action("actionConcluirEsboco")->trigger();

    QVERIFY(!m_window->isEditingSketch());
    QVERIFY(!viewer()->isSketchMode());
    QCOMPARE(m_window->document().sketches().size(), size_t(1));
    QVERIFY(m_window->document().isModified());
    QVERIFY(m_window->isWindowModified());
    // Árvore: documento > operações > esboço > entidades.
    QTreeWidgetItem* root = m_window->operationsTree()->topLevelItem(0);
    QCOMPARE(root->childCount(), 3);
    QTreeWidgetItem* sketch = findItem(m_window->operationsTree(), "esboco", "sk-1");
    QVERIFY(sketch);
    QCOMPARE(sketch->text(0), QStringLiteral("Esboço 1 (YZ)"));
    QCOMPARE(sketch->childCount(), 3);
    QVERIFY(sketch->child(0)->text(0).startsWith(QStringLiteral("Linha e-1")));
    QVERIFY(sketch->child(1)->text(0).startsWith(QStringLiteral("Retângulo e-2")));
    QVERIFY(sketch->child(2)->text(0).startsWith(QStringLiteral("Círculo e-3")));
    QCOMPARE(m_window->operationsTree()->currentItem(), sketch);
    auto* panel = m_window->findChild<QLabel*>("painelPropriedades");
    QVERIFY(panel->text().contains("Normal: (1,00; 0,00; 0,00)"));
    // Esboço continua visível fora da edição (camada do documento); ViewCube, eixos globais e
    // sólido voltam.
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->originAxes()));
    QVERIFY(action("actionVistaFrente")->isEnabled());
    action("actionVistaDireita")->trigger();
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), "E52_06_esboco_concluido_arvore");
    const SketchPlane p(PlaneType::YZ);
    QVERIFY(hasSketchColorNear(img, project(viewer(), p.toGlobal(85.0, 30.0)),
                               int(std::lround(2 * viewer()->devicePixelRatioF())), false));
}

void TestEsbocoUi::editarEsbocoExistentePelaArvore()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    QVERIFY(m_window->finishSketch());
    QTreeWidgetItem* sketch = findItem(m_window->operationsTree(), "esboco", "sk-1");
    emit m_window->operationsTree()->itemDoubleClicked(sketch, 0);
    QVERIFY(m_window->isEditingSketch());
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(3));
    // Novas entidades continuam a numeração (ids estáveis).
    tool("actionFerramentaLinha");
    QVERIFY(click(viewer(), at(0.0, 50.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(30.0, 50.0)));
    QVERIFY(m_window->editingSketch()->findEntity("e-4") != nullptr);
    QVERIFY(m_window->finishSketch());
    QCOMPARE(m_window->document().findSketch("sk-1")->entities().size(), size_t(4));
    // Segundo esboço recebe sk-2.
    m_window->nextPlane = PlaneType::XZ;
    action("actionNovoEsboco")->trigger();
    QCOMPARE(m_window->editingSketch()->id(), std::string("sk-2"));
    QVERIFY(m_window->finishSketch());
    QCOMPARE(m_window->document().sketches().size(), size_t(2));
}

void TestEsbocoUi::salvarEAbrirComEsbocos()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    QVERIFY(m_window->finishSketch());
    const Sketch saved = *m_window->document().findSketch("sk-1");

    m_window->nextSavePath = m_dir->filePath("com_esbocos.cadmec");
    action("actionSalvar")->trigger();
    QVERIFY(m_window->errors.isEmpty());
    QVERIFY(!m_window->document().isModified());

    action("actionNovo")->trigger();
    QVERIFY(m_window->document().sketches().empty());
    QCOMPARE(m_window->operationsTree()->topLevelItem(0)->childCount(), 0); // documento vazio

    m_window->nextOpenPath = m_dir->filePath("com_esbocos.cadmec");
    action("actionAbrir")->trigger();
    QVERIFY(m_window->errors.isEmpty());
    const Sketch* loaded = m_window->document().findSketch("sk-1");
    QVERIFY(loaded);
    QCOMPARE(loaded->entities().size(), saved.entities().size());
    for (size_t i = 0; i < saved.entities().size(); ++i)
    {
        QCOMPARE(loaded->entities()[i].id, saved.entities()[i].id);
        const auto a = dimensionsOf(saved.entities()[i]);
        const auto b = dimensionsOf(loaded->entities()[i]);
        for (size_t k = 0; k < a.size(); ++k)
            QCOMPARE(b[k].value, a[k].value);
    }
    QVERIFY(findItem(m_window->operationsTree(), "esboco", "sk-1"));
    QCOMPARE(findItem(m_window->operationsTree(), "esboco", "sk-1")->childCount(), 3);
}

void TestEsbocoUi::solidoEStepPreservados()
{
    const TopoDS_Shape before = m_window->part();
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    QVERIFY(m_window->part().IsSame(before));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->part().IsSame(before));
    QVERIFY(viewer()->shape().IsSame(before));
    cad::test::checkTestPart(cad::modeling::ShapeInspector::inspect(m_window->part()));

    m_window->nextExportPath = m_dir->filePath("solido.step");
    action("actionExportarStep")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = cad::modeling::ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 7); // esboços não entram no STEP do sólido
    QVERIFY(qAbs(s.volume - kExpectedVolume) <= kExpectedVolume * 1e-9);
}

void TestEsbocoUi::navegacaoESelecao3DForaDoEsboco()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    QVERIFY(m_window->finishSketch());
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));

    // Seleção de face do sólido volta a funcionar.
    QVERIFY(click(viewer(), toLogical(viewer(), project(viewer(), gp_Pnt(25.0, 15.0, 20.0)))));
    QTRY_VERIFY(m_window->statusBar()->currentMessage().startsWith("Face"));
    QCOMPARE(viewer()->selectedShapes().size(), 1);
    pauseBetweenClicks();
    // Rotação com o botão central volta a girar.
    const gp_Dir dir = viewer()->view()->Camera()->Direction();
    const QPointF c(viewer()->width() / 2.0, viewer()->height() / 2.0);
    QVERIFY(drag(viewer(), c, c + QPointF(100, 40), Qt::MiddleButton));
    QVERIFY(dir.Angle(viewer()->view()->Camera()->Direction()) > 0.1);
    saveEvidence(m_window->grab().toImage(), "E52_07_navegacao_3d_apos_esboco");
}

void TestEsbocoUi::viewCubeEVistasAposEsboco()
{
    action("actionNovoEsboco")->trigger();
    action("actionCancelarEsboco")->trigger();
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    struct Case { const char* action; gp_Dir dir; } cases[] = {
        {"actionVistaFrente", gp_Dir(0, 1, 0)},
        {"actionVistaSuperior", gp_Dir(0, 0, -1)},
        {"actionVistaDireita", gp_Dir(-1, 0, 0)},
        {"actionVistaIsometrica", gp_Dir(-1, 1, -1)},
    };
    for (const Case& c : cases)
    {
        action(c.action)->trigger();
        QVERIFY2(viewer()->view()->Camera()->Direction().Angle(c.dir) < 1e-3, c.action);
    }
    // Clique na face SUPERIOR do ViewCube (localizada por detecção; clique real do mouse).
    const auto& ctx = viewer()->aisContext();
    int w = 0, h = 0;
    viewer()->view()->Window()->Size(w, h);
    long sx = 0, sy = 0, n = 0;
    for (int y = 0; y < h / 3; y += 3)
        for (int x = w * 2 / 3; x < w; x += 3)
        {
            ctx->MoveTo(x, y, viewer()->view(), false);
            auto owner = occ::down_cast<AIS_ViewCubeOwner>(ctx->DetectedOwner());
            if (!owner.IsNull() && owner->MainOrientation() == V3d_Zpos)
            {
                sx += x;
                sy += y;
                ++n;
            }
        }
    ctx->ClearDetected(false);
    viewer()->ResetPreviousMoveTo();
    QVERIFY(n > 0);
    QVERIFY(click(viewer(), toLogical(viewer(), QPoint(int(sx / n), int(sy / n)))));
    QTRY_VERIFY_WITH_TIMEOUT(viewer()->view()->Camera()->Direction().Angle(gp_Dir(0, 0, -1)) < 1e-3, 5000);
}

void TestEsbocoUi::fechamentoDuranteEdicao()
{
    action("actionNovoEsboco")->trigger();
    drawThreeEntities();
    // Fechar com esboço em edição: a edição é descartada e a janela fecha normalmente.
    QVERIFY(m_window->close());
    QVERIFY(!m_window->isVisible());
    QVERIFY(!m_window->isEditingSketch());
    QVERIFY(m_window->document().sketches().empty());
    delete m_window;
    m_window = nullptr;
}

QTEST_MAIN(TestEsbocoUi)
#include "test_esboco_ui.moc"
