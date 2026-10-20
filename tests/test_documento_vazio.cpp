// E5.2.1: o CadMecanico inicia com documento vazio; esboços em documento vazio; o sólido e os
// outros esboços ficam ocultos durante a edição de um esboço e voltam ao concluir/cancelar.
// Contexto OpenGL real.
#include "ViewerTestUtils.h"

#include "app/MainWindow.h"
#include "document/DocumentFile.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QFile>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <Graphic3d_Camera.hxx>
#include <gp_Dir.hxx>

#include <filesystem>
#include <optional>

using cad::app::MainWindow;
using cad::document::CadDocument;
using cad::document::DocumentFile;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kExpectedVolume = 100.0 * 60.0 * 20.0 - 3.14159265358979323846 * 10.0 * 10.0 * 20.0;

class TestableMainWindow : public MainWindow
{
public:
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    QString nextSavePath;
    QString nextOpenPath;
    QString nextExportPath;
    int exportPathRequests = 0;
    QStringList errors;
    QStringList infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    QString askExportPath(const QString&) override
    {
        ++exportPathRequests;
        return nextExportPath;
    }
    bool confirmOverwrite(const QString&) override { return true; }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
};

bool hasColorNear(const QImage& img, QPoint c, int r, bool (*pred)(QRgb))
{
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (pred(pixelAt(img, c + QPoint(dx, dy))))
                return true;
    return false;
}

bool isRed(QRgb p) { return qRed(p) > 170 && qGreen(p) < 90 && qBlue(p) < 90; }
bool isGreen(QRgb p) { return qGreen(p) > 100 && qRed(p) < 90 && qBlue(p) < 90; }
bool isBlue(QRgb p) { return qBlue(p) > 170 && qRed(p) < 90 && qGreen(p) < 90; }
bool isSketchBlue(QRgb p) { return qBlue(p) > 140 && qBlue(p) - qRed(p) > 80 && qGreen(p) < 140; }

} // namespace

class TestDocumentoVazio : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void inicializacaoComAreaVazia();
    void novoDocumentoSemOperacoes();
    void novoEsbocoNosPlanos_data();
    void novoEsbocoNosPlanos();
    void retanguloECirculoEmDocumentoVazio();
    void preVisualizacaoEntreCliques();
    void concluirSemCriarSolido();
    void salvarEAbrirSomenteEsbocos();
    void abrirDocumentosAntigosComBlocoEFuro_data();
    void abrirDocumentosAntigosComBlocoEFuro();
    void edicaoOcultaSolidoEOutrosEsbocos();
    void restauraAoConcluirOuCancelar();
    void exportarStepComSolido();
    void exportarStepSemSolido();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        const SketchPlane& plane = m_window->editingSketch()->plane();
        return toLogical(viewer(), project(viewer(), plane.toGlobal(u, v)));
    }
    void clickAt(double u, double v)
    {
        QVERIFY(click(viewer(), at(u, v)));
        pauseBetweenClicks();
    }
    // Retângulo (10,10)-(50,40) e círculo de centro (70,25) e raio 10, pelo mouse.
    void drawRectangleAndCircle();
    // Quantos pixels diferem de um visualizador vazio com a mesma câmera.
    int differenceFromEmptyViewer();

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestDocumentoVazio::init()
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
    QVERIFY(waitViewerReady(viewer()));
    pauseBetweenClicks();
}

void TestDocumentoVazio::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestDocumentoVazio::drawRectangleAndCircle()
{
    action("actionFerramentaRetangulo")->trigger();
    clickAt(10.0, 10.0);
    clickAt(50.0, 40.0);
    action("actionFerramentaCirculo")->trigger();
    clickAt(70.0, 25.0);
    clickAt(80.0, 25.0);
    action("actionFerramentaSelecionar")->trigger();
}

int TestDocumentoVazio::differenceFromEmptyViewer()
{
    const QImage img = viewer()->grabViewerImage();
    const QImage ref = backgroundReference(viewer()); // visualizador novo, sem conteúdo
    return diff(img, ref).count;
}

void TestDocumentoVazio::inicializacaoComAreaVazia()
{
    // Documento vazio: nenhuma operação, nenhum sólido, nenhum esboço.
    QVERIFY(!m_window->document().hasPart());
    QVERIFY(m_window->document().operations().empty());
    QVERIFY(m_window->document().sketches().empty());
    QVERIFY(m_window->part().IsNull());
    QVERIFY(viewer()->shape().IsNull());
    QVERIFY(viewer()->shapePresentation().IsNull());
    QVERIFY(viewer()->documentSketchPresentations().empty());
    QVERIFY(m_window->windowTitle().startsWith(QStringLiteral("Sem título")));
    QVERIFY(!m_window->isWindowModified());
    QCOMPARE(m_window->operationsTree()->topLevelItemCount(), 1);
    QCOMPARE(m_window->operationsTree()->topLevelItem(0)->childCount(), 0);
    QVERIFY(m_window->statusBar()->currentMessage().contains("Open CASCADE"));

    // Área gráfica: igual a um visualizador vazio (fundo, eixos de referência, triedro, ViewCube).
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), "E521_01_inicializacao_vazia");
    const int different = differenceFromEmptyViewer();
    qInfo("Pixels diferentes de um visualizador vazio: %d de %d", different, img.width() * img.height());
    QVERIFY(different < img.width() * img.height() / 500);
    // Eixos de referência X/Y/Z na origem, visíveis no centro da área.
    const QPoint o = project(viewer(), gp_Pnt(0, 0, 0));
    QVERIFY(QRect(QPoint(0, 0), img.size()).adjusted(img.width() / 4, img.height() / 4, -img.width() / 4,
                                                     -img.height() / 4).contains(o));
    const int r = int(std::lround(25 * viewer()->devicePixelRatioF()));
    const double s = viewer()->devicePixelRatioF() * 30.0;
    auto axisPoint = [&](const gp_Dir& d) {
        // Direção na tela do eixo d a partir da origem.
        const QPoint e = project(viewer(), gp_Pnt(d.X(), d.Y(), d.Z()));
        const QPointF v(e - o);
        const double len = std::hypot(v.x(), v.y());
        return (o + (v / len * s).toPoint());
    };
    QVERIFY(hasColorNear(img, axisPoint(gp::DX()), r / 3, isRed));
    QVERIFY(hasColorNear(img, axisPoint(gp::DY()), r / 3, isGreen));
    QVERIFY(hasColorNear(img, axisPoint(gp::DZ()), r / 3, isBlue));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    QVERIFY(!action("actionExportarStep")->isEnabled());
    QVERIFY(!action("actionEditarParametros")->isEnabled());
    QVERIFY(action("actionNovoEsboco")->isEnabled());
}

void TestDocumentoVazio::novoDocumentoSemOperacoes()
{
    action("actionExemploBlocoFuro")->trigger();
    QVERIFY(m_window->document().hasPart());
    QVERIFY(m_window->applyParameters({120.0, 60.0, 20.0, 10.0})); // modificado
    action("actionNovo")->trigger();                                // pergunta -> descartar
    QVERIFY(!m_window->document().hasPart());
    QVERIFY(m_window->document().operations().empty());
    QVERIFY(m_window->document().sketches().empty());
    QVERIFY(viewer()->shape().IsNull());
    QVERIFY(waitFrames(viewer()));
    QVERIFY(differenceFromEmptyViewer() < viewer()->grabViewerImage().width() * 10);
    saveEvidence(m_window->grab().toImage(), "E521_02_novo_documento_vazio");
}

void TestDocumentoVazio::novoEsbocoNosPlanos_data()
{
    QTest::addColumn<int>("plane");
    QTest::newRow("XY") << int(PlaneType::XY);
    QTest::newRow("XZ") << int(PlaneType::XZ);
    QTest::newRow("YZ") << int(PlaneType::YZ);
}

void TestDocumentoVazio::novoEsbocoNosPlanos()
{
    QFETCH(int, plane);
    m_window->nextPlane = PlaneType(plane);
    action("actionNovoEsboco")->trigger();
    QVERIFY(m_window->isEditingSketch());
    const SketchPlane& p = m_window->editingSketch()->plane();
    QVERIFY(viewer()->view()->Camera()->Direction().IsOpposite(p.normal(), 1e-6));
    QVERIFY(viewer()->view()->Camera()->OrthogonalizedUp().IsEqual(p.verticalAxis(), 1e-6));
    QVERIFY(viewer()->view()->Camera()->IsOrthographic());
    for (const char* a : {"actionFerramentaLinha", "actionFerramentaRetangulo", "actionFerramentaCirculo"})
        QVERIFY(action(a)->isEnabled());
    // Eixos locais H/V visíveis e com escala útil mesmo sem sólido (região padrão de 100 mm).
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), QStringLiteral("E521_03_esboco_vazio_%1").arg(QTest::currentDataTag()));
    const int r = int(std::lround(2 * viewer()->devicePixelRatioF()));
    QVERIFY(hasColorNear(img, project(viewer(), p.toGlobal(50.0, 0.0)), r, isRed));
    QVERIFY(hasColorNear(img, project(viewer(), p.toGlobal(0.0, 50.0)), r, isGreen));
    const QPoint a = project(viewer(), p.toGlobal(0, 0)), b = project(viewer(), p.toGlobal(100, 0));
    QVERIFY(b.x() - a.x() > img.width() / 4); // 100 mm ocupam boa parte da largura
    QVERIFY(QRect(QPoint(0, 0), img.size()).contains(b));
}

void TestDocumentoVazio::retanguloECirculoEmDocumentoVazio()
{
    action("actionNovoEsboco")->trigger();
    drawRectangleAndCircle();
    const Sketch* s = m_window->editingSketch();
    QCOMPARE(s->entities().size(), size_t(2));
    const auto rect = std::get<SketchRectangle>(s->findEntity("e-1")->geometry);
    const auto circle = std::get<SketchCircle>(s->findEntity("e-2")->geometry);
    qInfo("Retangulo (%.1f, %.1f) %.1f x %.1f; circulo (%.1f, %.1f) R %.1f", rect.x, rect.y, rect.width,
          rect.height, circle.cx, circle.cy, circle.radius);
    QVERIFY(std::fabs(rect.x - 10.0) <= 0.5 && std::fabs(rect.y - 10.0) <= 0.5);
    QVERIFY(std::fabs(rect.width - 40.0) <= 0.5 && std::fabs(rect.height - 30.0) <= 0.5);
    QVERIFY(std::fabs(circle.cx - 70.0) <= 0.5 && std::fabs(circle.cy - 25.0) <= 0.5);
    QVERIFY(std::fabs(circle.radius - 10.0) <= 0.5);
    m_window->selectSketchEntity(QString());
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    const SketchPlane& p = s->plane();
    const int r = int(std::lround(2 * viewer()->devicePixelRatioF()));
    QVERIFY(hasColorNear(img, project(viewer(), p.toGlobal(30.0, 40.0)), r, isSketchBlue));
    QVERIFY(hasColorNear(img, project(viewer(), p.toGlobal(80.0, 25.0)), r, isSketchBlue));
    saveEvidence(m_window->grab().toImage(), "E521_04_retangulo_circulo_documento_vazio");
}

void TestDocumentoVazio::preVisualizacaoEntreCliques()
{
    action("actionNovoEsboco")->trigger();
    action("actionFerramentaRetangulo")->trigger();
    clickAt(10.0, 10.0);
    sendMouse(viewer(), QEvent::MouseMove, at(50.0, 40.0), Qt::NoButton, Qt::NoButton);
    QVERIFY(waitFrames(viewer(), 2));
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(img, "E521_04a_pre_visualizacao_retangulo");
    // Lado inferior e lado direito da pré-visualização (cinza escuro) aparecem antes do 2º clique.
    const SketchPlane& p = m_window->editingSketch()->plane();
    const int r = int(std::lround(2 * viewer()->devicePixelRatioF()));
    auto dark = [](QRgb c) { return luminance(c) < 110; };
    bool bottom = false, right = false;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
        {
            bottom |= dark(pixelAt(img, project(viewer(), p.toGlobal(30.0, 10.0)) + QPoint(dx, dy)));
            right |= dark(pixelAt(img, project(viewer(), p.toGlobal(50.0, 25.0)) + QPoint(dx, dy)));
        }
    QVERIFY(bottom && right);
    QVERIFY(m_window->editingSketch()->entities().empty()); // ainda não criado
    // Esc remove a pré-visualização.
    QTest::keyClick(viewer(), Qt::Key_Escape);
    QVERIFY(waitFrames(viewer(), 2));
    const QImage after = viewer()->grabViewerImage();
    bool still = false;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            still |= dark(pixelAt(after, project(viewer(), p.toGlobal(30.0, 10.0)) + QPoint(dx, dy)));
    QVERIFY(!still);
}

void TestDocumentoVazio::concluirSemCriarSolido()
{
    action("actionNovoEsboco")->trigger();
    drawRectangleAndCircle();
    action("actionConcluirEsboco")->trigger();
    QVERIFY(!m_window->isEditingSketch());
    QCOMPARE(m_window->document().sketches().size(), size_t(1));
    QVERIFY(!m_window->document().hasPart());
    QVERIFY(m_window->document().operations().empty());
    QVERIFY(m_window->part().IsNull());
    QVERIFY(viewer()->shape().IsNull());
    QVERIFY(viewer()->shapePresentation().IsNull());
    QCOMPARE(viewer()->documentSketchPresentations().size(), size_t(1));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->documentSketchPresentations()[0]));
    QVERIFY(!action("actionExportarStep")->isEnabled());
    QTreeWidgetItem* root = m_window->operationsTree()->topLevelItem(0);
    QCOMPARE(root->childCount(), 1); // apenas o esboço
    QCOMPARE(root->child(0)->data(0, MainWindow::kItemKindRole).toString(), QStringLiteral("esboco"));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E521_05_esboco_concluido_sem_solido");
}

void TestDocumentoVazio::salvarEAbrirSomenteEsbocos()
{
    action("actionNovoEsboco")->trigger();
    drawRectangleAndCircle();
    QVERIFY(m_window->finishSketch());
    const QString path = m_dir->filePath("somente_esbocos.cadmec");
    m_window->nextSavePath = path;
    action("actionSalvar")->trigger();
    QVERIFY(m_window->errors.isEmpty());
    QVERIFY(QFile::exists(path));

    action("actionNovo")->trigger();
    QVERIFY(m_window->document().sketches().empty());
    m_window->nextOpenPath = path;
    action("actionAbrir")->trigger();
    QVERIFY(m_window->errors.isEmpty());
    QVERIFY(!m_window->document().hasPart());
    QVERIFY(viewer()->shape().IsNull());
    QCOMPARE(m_window->document().sketches().size(), size_t(1));
    QCOMPARE(m_window->document().sketches()[0].entities().size(), size_t(2));
    QCOMPARE(viewer()->documentSketchPresentations().size(), size_t(1));
    QCOMPARE(m_window->operationsTree()->topLevelItem(0)->childCount(), 1);
    // Aberto em vista isométrica, com o esboço enquadrado e desenhado.
    QVERIFY(viewer()->view()->Camera()->Direction().IsEqual(gp_Dir(-1, 1, -1), 1e-6));
    QVERIFY(waitFrames(viewer(), 2));
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), "E521_06_aberto_somente_esbocos");
    const Sketch& s = m_window->document().sketches()[0];
    const QRect inner = QRect(QPoint(0, 0), img.size()).adjusted(2, 2, -2, -2);
    for (const gp_Pnt2d& q : {gp_Pnt2d(10, 10), gp_Pnt2d(50, 40), gp_Pnt2d(80, 25)})
        QVERIFY(inner.contains(project(viewer(), s.plane().toGlobal(q))));
    const int r = int(std::lround(2 * viewer()->devicePixelRatioF()));
    QVERIFY(hasColorNear(img, project(viewer(), s.plane().toGlobal(30.0, 10.0)), r, isSketchBlue));
}

void TestDocumentoVazio::abrirDocumentosAntigosComBlocoEFuro_data()
{
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<int>("sketches");
    QTest::newRow("E5.1 esquema 1") << QStringLiteral("documento_e51_v1.cadmec") << 0;
    QTest::newRow("E5.2 esquema 2") << QStringLiteral("documento_e52_v2.cadmec") << 1;
}

void TestDocumentoVazio::abrirDocumentosAntigosComBlocoEFuro()
{
    QFETCH(QString, fixture);
    QFETCH(int, sketches);
    // Cópia em diretório temporário: o arquivo original não é alterado.
    const QString source = QStringLiteral(CADMEC_TEST_DATA_DIR "/") + fixture;
    const QString path = m_dir->filePath(fixture);
    QVERIFY(QFile::copy(source, path));
    m_window->nextOpenPath = path;
    action("actionAbrir")->trigger();
    QVERIFY(m_window->errors.isEmpty());
    QVERIFY(m_window->document().hasPart());
    QCOMPARE(m_window->document().sketches().size(), size_t(sketches));
    QVERIFY(!viewer()->shape().IsNull());
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QCOMPARE(cad::modeling::ShapeInspector::inspect(m_window->part()).faces, 7);
    QVERIFY(action("actionExportarStep")->isEnabled());
    QVERIFY(action("actionEditarParametros")->isEnabled());
    // O sólido aparece no framebuffer.
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    const DiffResult d = diff(img, backgroundReference(viewer()));
    QVERIFY(double(d.count) / (img.width() * img.height()) > 0.1);
    QFile original(source), copy(path);
    QVERIFY(original.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), original.readAll()); // abrir não regrava o arquivo
    saveEvidence(m_window->grab().toImage(), QStringLiteral("E521_07_documento_antigo_%1").arg(sketches + 1));
}

void TestDocumentoVazio::edicaoOcultaSolidoEOutrosEsbocos()
{
    action("actionExemploBlocoFuro")->trigger();
    action("actionNovoEsboco")->trigger();
    drawRectangleAndCircle();
    QVERIFY(m_window->finishSketch()); // sk-1 no documento com sólido
    const auto& ctx = viewer()->aisContext();
    QVERIFY(ctx->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(ctx->IsDisplayed(viewer()->documentSketchPresentations()[0]));

    m_window->nextPlane = PlaneType::XZ;
    action("actionNovoEsboco")->trigger(); // sk-2
    QVERIFY(m_window->isEditingSketch());
    // Sólido, outros esboços, eixos globais e ViewCube ocultos; documento inalterado.
    QVERIFY(!ctx->IsDisplayed(viewer()->shapePresentation()));
    for (const auto& prs : viewer()->documentSketchPresentations())
        QVERIFY(!ctx->IsDisplayed(prs));
    QVERIFY(!ctx->IsDisplayed(viewer()->originAxes()));
    QVERIFY(!ctx->IsDisplayed(viewer()->viewCube()));
    QVERIFY(m_window->document().hasPart());
    // O framebuffer não mostra o sólido: um ponto no meio da face frontal (y = 0) fica com o
    // fundo, como no visualizador vazio de referência.
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), "E521_08_edicao_oculta_solido");
    const QImage ref = backgroundReference(viewer());
    const QPoint inside = project(viewer(), gp_Pnt(50.0, 0.0, 10.0));
    QVERIFY(!differs(meanAround(img, inside, 2), meanAround(ref, inside, 2), 12));
    // Ferramentas disponíveis com o sólido oculto.
    action("actionFerramentaLinha")->trigger();
    clickAt(-20.0, 30.0);
    clickAt(20.0, 30.0);
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
}

void TestDocumentoVazio::restauraAoConcluirOuCancelar()
{
    action("actionExemploBlocoFuro")->trigger();
    const TopoDS_Shape solid = m_window->part();
    const auto& ctx = viewer()->aisContext();
    for (int round = 0; round < 2; ++round)
    {
        action("actionNovoEsboco")->trigger();
        QVERIFY(!ctx->IsDisplayed(viewer()->shapePresentation()));
        drawRectangleAndCircle();
        if (round == 0)
            action("actionCancelarEsboco")->trigger();
        else
            action("actionConcluirEsboco")->trigger();
        QVERIFY(!m_window->isEditingSketch());
        // Sólido e eixos de referência de volta, selecionáveis; nenhum sólido novo criado.
        QVERIFY(ctx->IsDisplayed(viewer()->shapePresentation()));
        QVERIFY(ctx->IsDisplayed(viewer()->originAxes()));
        QVERIFY(ctx->IsDisplayed(viewer()->viewCube()));
        QVERIFY(m_window->part().IsSame(solid));
        QCOMPARE(m_window->document().operations().size(), size_t(2));
        // Sem mudar a câmera (ainda perpendicular ao plano do esboço): o sólido é redesenhado.
        QVERIFY(waitFrames(viewer(), 2));
        {
            const QImage now = viewer()->grabViewerImage();
            saveEvidence(now, QStringLiteral("E521_09a_apos_%1_sem_mudar_vista").arg(round == 0 ? "cancelar" : "concluir"));
            const DiffResult d0 = diff(now, backgroundReference(viewer()));
            qInfo("Apos %s: %d pixels diferentes do vazio", round == 0 ? "cancelar" : "concluir", d0.count);
            QVERIFY(double(d0.count) / (now.width() * now.height()) > 0.1);
            const QPoint c = project(viewer(), gp_Pnt(25.0, 15.0, 20.0));
            QVERIFY(QRect(QPoint(0, 0), now.size()).contains(c));
        }
        action("actionVistaIsometrica")->trigger();
        QVERIFY(waitFrames(viewer()));
        const QImage img = viewer()->grabViewerImage();
        const DiffResult d = diff(img, backgroundReference(viewer()));
        QVERIFY(double(d.count) / (img.width() * img.height()) > 0.1);
        QVERIFY(click(viewer(), toLogical(viewer(), project(viewer(), gp_Pnt(25.0, 15.0, 20.0)))));
        QTRY_COMPARE(viewer()->selectedShapes().size(), 1);
        pauseBetweenClicks();
        viewer()->clearSelection();
    }
    QCOMPARE(m_window->document().sketches().size(), size_t(1)); // só o concluído
    saveEvidence(m_window->grab().toImage(), "E521_09_solido_restaurado");
}

void TestDocumentoVazio::exportarStepComSolido()
{
    action("actionExemploBlocoFuro")->trigger();
    action("actionNovoEsboco")->trigger();
    drawRectangleAndCircle();
    QVERIFY(m_window->finishSketch());
    QVERIFY(action("actionExportarStep")->isEnabled());
    m_window->nextExportPath = m_dir->filePath("solido.step");
    action("actionExportarStep")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = cad::modeling::ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 7);
    QVERIFY(qAbs(s.volume - kExpectedVolume) <= kExpectedVolume * 1e-9);
}

void TestDocumentoVazio::exportarStepSemSolido()
{
    // Documento vazio e documento só com esboços: ação desabilitada; chamada direta é recusada
    // com mensagem clara, sem pedir caminho e sem criar arquivo.
    for (int round = 0; round < 2; ++round)
    {
        if (round == 1)
        {
            action("actionNovoEsboco")->trigger();
            drawRectangleAndCircle();
            QVERIFY(m_window->finishSketch());
        }
        QVERIFY(!action("actionExportarStep")->isEnabled());
        m_window->nextExportPath = m_dir->filePath("nada.step");
        m_window->exportStep();
        QCOMPARE(m_window->exportPathRequests, 0);
        QCOMPARE(m_window->errors.size(), 2 * round + 1); // exportStep (e exportStepTo na rodada anterior)
        QVERIFY(m_window->errors.last().contains(QStringLiteral("não contém sólido")));
        QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("não contém sólido")));
        QVERIFY(!QFile::exists(m_window->nextExportPath));
        QVERIFY(!m_window->exportStepTo(m_window->nextExportPath));
        QVERIFY(!QFile::exists(m_window->nextExportPath));
    }
}

QTEST_MAIN(TestDocumentoVazio)
#include "test_documento_vazio.moc"
