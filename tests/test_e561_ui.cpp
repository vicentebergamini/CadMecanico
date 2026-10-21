// E5.6.1 na interface real, com contexto OpenGL real: interação do mouse nas ferramentas de
// esboço com cliques independentes como num mouse físico (o botão é solto entre os pontos e o
// clique pode ter pequeno tremido), pré-visualização dinâmica com o cursor sem botão
// pressionado, desenho por arrasto, arcos com três cliques, Esc, troca de ferramenta, clique
// direito, Fillet/Chamfer 2D, esboço sobre face com extrusão do perfil e persistência.
#include "ViewerTestUtils.h"

#include "app/MainWindow.h"
#include "document/DocumentFile.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QRegularExpression>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <cmath>
#include <optional>

using cad::app::MainWindow;
using cad::document::EndCondition;
using cad::document::ExtrudeMode;
using cad::document::ExtrudeParameters;
using cad::modeling::ShapeInspector;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kPi = 3.14159265358979323846;

class TestableMainWindow : public MainWindow
{
public:
    std::optional<double> nextCornerValue = 5.0;
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    std::optional<ExtrudeParameters> nextExtrusion;
    QString nextSavePath, nextOpenPath;
    QStringList errors, infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    std::optional<double> askCornerValue(EntityType, double) override { return nextCornerValue; }
    std::optional<SketchChamfer> askChamfer2D(const SketchChamfer& initial) override
    {
        if (!nextCornerValue)
            return std::nullopt;
        SketchChamfer c = initial;
        c.distance = *nextCornerValue;
        return c;
    }
    std::optional<ExtrudeParameters> askExtrusion(const ExtrudeParameters& initial, bool) override
    {
        if (!nextExtrusion)
            return std::nullopt;
        ExtrudeParameters p = *nextExtrusion;
        p.sketchId = initial.sketchId;
        if (p.mode != ExtrudeMode::NewBody && p.targetId.empty())
            p.targetId = initial.targetId;
        return p;
    }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    bool confirmOverwrite(const QString&) override { return true; }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
};

bool approx(double a, double b, double tol = 0.35)
{
    return std::fabs(a - b) <= tol;
}

} // namespace

class TestE561Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void linhaComDoisCliquesIndependentes();
    void retanguloComPreviaDinamica();
    void circuloComDoisCliques();
    void arcoComTresCliques();
    void desenhoPorArrasto();
    void escECancelamento();
    void trocaDeFerramentaCancelaPonto();
    void cliqueDireitoNaoInterfere();
    void focoAoSelecionarFerramenta();
    void filletEChamfer2DComTremido();
    void esbocoSobreFaceEExtrusao();
    void persistenciaDasEntidades();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        return toLogical(viewer(), project(viewer(), m_window->editingSketch()->plane().toGlobal(u, v)));
    }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    // Clique físico: move sem botão, pressiona, desloca 'jitter' pixels lógicos com o botão
    // pressionado e solta (o botão não permanece pressionado entre os pontos).
    bool physicalClick(QPointF pos, QPointF jitter = QPointF(), Qt::MouseButton button = Qt::LeftButton)
    {
        sendMouse(viewer(), QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
        if (!waitFrames(viewer()))
            return false;
        sendMouse(viewer(), QEvent::MouseButtonPress, pos, button, button);
        if (!jitter.isNull())
            sendMouse(viewer(), QEvent::MouseMove, pos + jitter, Qt::NoButton, button);
        sendMouse(viewer(), QEvent::MouseButtonRelease, pos + jitter, button, Qt::NoButton);
        const bool ok = waitFrames(viewer(), 2);
        pauseBetweenClicks();
        return ok;
    }
    // Movimento sem botão pressionado (pré-visualização) em alguns passos.
    bool hover(QPointF from, QPointF to)
    {
        for (int i = 1; i <= 5; ++i)
        {
            sendMouse(viewer(), QEvent::MouseMove, from + (to - from) * (i / 5.0), Qt::NoButton, Qt::NoButton);
            if (!waitFrames(viewer()))
                return false;
        }
        return true;
    }
    void tool(const char* name) { action(name)->trigger(); }
    const Sketch* sketch() const { return m_window->editingSketch(); }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE561Ui::init()
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
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    pauseBetweenClicks();
}

void TestE561Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE561Ui::linhaComDoisCliquesIndependentes()
{
    tool("actionFerramentaLinha");
    // Primeiro clique com tremido de 6 px (antes da E5.6.1 era descartado): ponto pendente
    // registrado na posição do pressionamento.
    QVERIFY(physicalClick(at(10, 10), QPointF(6, 3)));
    QVERIFY(viewer()->hasPendingSketchPoint());
    QVERIFY(sketch()->entities().empty());
    QVERIFY(hover(at(10, 10), at(50, 20)));
    QVERIFY(physicalClick(at(60, 10), QPointF(-4, 5)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    const auto& l = std::get<SketchLine>(sketch()->entities()[0].geometry);
    qInfo("Linha: (%.2f, %.2f) -> (%.2f, %.2f)", l.x1, l.y1, l.x2, l.y2);
    QVERIFY(approx(l.x1, 10) && approx(l.y1, 10) && approx(l.x2, 60) && approx(l.y2, 10));
    QVERIFY(!viewer()->hasPendingSketchPoint());
    QVERIFY(viewer()->sketchTool() == cad::viewer::SketchTool::Line); // ferramenta continua ativa
    // Ferramenta permanece ativa: nova linha sem reativar.
    QVERIFY(physicalClick(at(10, 30)));
    QVERIFY(physicalClick(at(40, 30)));
    QCOMPARE(sketch()->entities().size(), size_t(2));
}

void TestE561Ui::retanguloComPreviaDinamica()
{
    tool("actionFerramentaRetangulo");
    QVERIFY(physicalClick(at(20, 20), QPointF(3, -2)));
    QVERIFY(viewer()->hasPendingSketchPoint());
    // Pré-visualização acompanha o cursor sem botão pressionado.
    QVERIFY(hover(at(20, 20), at(50, 40)));
    QVERIFY(!viewer()->sketchPreviewPresentation().IsNull());
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    Bnd_Box first;
    BRepBndLib::Add(viewer()->sketchPreviewPresentation()->Shape(), first);
    QVERIFY(hover(at(50, 40), at(80, 60)));
    Bnd_Box second;
    BRepBndLib::Add(viewer()->sketchPreviewPresentation()->Shape(), second);
    double x0, y0, z0, x1, y1, z1, X0, Y0, Z0, X1, Y1, Z1;
    first.Get(x0, y0, z0, x1, y1, z1);
    second.Get(X0, Y0, Z0, X1, Y1, Z1);
    qInfo("Prévia: [%.1f; %.1f] -> [%.1f; %.1f]", x1, y1, X1, Y1);
    QVERIFY(approx(x1, 50, 1.0) && approx(y1, 40, 1.0) && approx(X1, 80, 1.0) && approx(Y1, 60, 1.0));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E561_01_previa_retangulo");
    QVERIFY(physicalClick(at(80, 60), QPointF(5, 0)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    const auto& r = std::get<SketchRectangle>(sketch()->entities()[0].geometry);
    QVERIFY(approx(r.x, 20) && approx(r.y, 20) && approx(r.width, 60) && approx(r.height, 40));
    QVERIFY(viewer()->sketchPreviewPresentation().IsNull());
}

void TestE561Ui::circuloComDoisCliques()
{
    tool("actionFerramentaCirculo");
    QVERIFY(physicalClick(at(50, 30), QPointF(0, 7)));
    QVERIFY(hover(at(50, 30), at(58, 30)));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    QVERIFY(physicalClick(at(60, 30)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    const auto& c = std::get<SketchCircle>(sketch()->entities()[0].geometry);
    QVERIFY(approx(c.cx, 50) && approx(c.cy, 30) && approx(c.radius, 10));
}

void TestE561Ui::arcoComTresCliques()
{
    tool("actionFerramentaArco");
    QVERIFY(physicalClick(at(50, 30), QPointF(4, 4))); // centro
    QVERIFY(hover(at(50, 30), at(70, 30)));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    QVERIFY(physicalClick(at(70, 30))); // início (raio 20)
    QVERIFY(sketch()->entities().empty());
    QVERIFY(hover(at(70, 30), at(50, 50)));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    QVERIFY(physicalClick(at(50, 50), QPointF(-3, 2))); // fim
    QCOMPARE(sketch()->entities().size(), size_t(1));
    const auto& a = std::get<SketchArc>(sketch()->entities()[0].geometry);
    qInfo("Arco: centro (%.2f, %.2f) R %.2f, %.1f° -> %.1f°", a.cx, a.cy, a.radius, a.startAngle, a.endAngle);
    QVERIFY(approx(a.cx, 50) && approx(a.cy, 30) && approx(a.radius, 20));
    QVERIFY(approx(a.startAngle, 0.0, 1.0) && approx(a.endAngle, 90.0, 1.5));
    // Propriedades da entidade com a unidade de cada medida (ângulos em graus, opção em texto).
    const QString panel = m_window->findChild<QLabel*>("painelPropriedades")->text();
    QVERIFY2(panel.contains(QRegularExpression(QStringLiteral("Ângulo final: \\d+,\\d\\d°")))
                 && panel.contains(QStringLiteral("Sentido horário: não"))
                 && panel.contains(QRegularExpression(QStringLiteral("Raio: \\d+,\\d\\d mm"))),
             qPrintable(panel));
}

void TestE561Ui::desenhoPorArrasto()
{
    // Alternativa: pressionar no primeiro canto, arrastar (prévia) e soltar no oposto.
    tool("actionFerramentaRetangulo");
    const QPointF a = at(10, 10), b = at(40, 30);
    sendMouse(viewer(), QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton);
    sendMouse(viewer(), QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i)
    {
        sendMouse(viewer(), QEvent::MouseMove, a + (b - a) * (i / 8.0), Qt::NoButton, Qt::LeftButton);
        QVERIFY(waitFrames(viewer()));
    }
    QVERIFY(viewer()->hasPendingSketchPoint());
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    sendMouse(viewer(), QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
    QVERIFY(waitFrames(viewer(), 2));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    const auto& r = std::get<SketchRectangle>(sketch()->entities()[0].geometry);
    QVERIFY(approx(r.x, 10) && approx(r.y, 10) && approx(r.width, 30) && approx(r.height, 20));
    pauseBetweenClicks();
    // Linha: primeiro ponto por clique, segundo por arrasto curto até o ponto final.
    tool("actionFerramentaLinha");
    QVERIFY(physicalClick(at(60, 10)));
    QVERIFY(drag(viewer(), at(60, 12), at(90, 10), Qt::LeftButton));
    QCOMPARE(sketch()->entities().size(), size_t(2));
    const auto& l = std::get<SketchLine>(sketch()->entities()[1].geometry);
    QVERIFY(approx(l.x1, 60) && approx(l.y1, 10) && approx(l.x2, 90) && approx(l.y2, 10));
}

void TestE561Ui::escECancelamento()
{
    tool("actionFerramentaLinha");
    QVERIFY(physicalClick(at(10, 10)));
    QVERIFY(hover(at(10, 10), at(30, 30)));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    QTest::keyClick(viewer(), Qt::Key_Escape);
    QVERIFY(!viewer()->hasPendingSketchPoint());
    QVERIFY(viewer()->sketchPreviewPresentation().IsNull());
    QVERIFY(physicalClick(at(30, 30))); // novo primeiro ponto
    QVERIFY(sketch()->entities().empty());
    QVERIFY(physicalClick(at(50, 30)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    // Cancelar o esboço descarta tudo sem alterar o documento.
    action("actionCancelarEsboco")->trigger();
    QVERIFY(!m_window->isEditingSketch());
    QVERIFY(m_window->document().sketches().empty());
}

void TestE561Ui::trocaDeFerramentaCancelaPonto()
{
    tool("actionFerramentaRetangulo");
    QVERIFY(physicalClick(at(10, 10)));
    QVERIFY(viewer()->hasPendingSketchPoint());
    tool("actionFerramentaCirculo"); // troca: ponto pendente descartado, nova ferramenta pronta
    QVERIFY(!viewer()->hasPendingSketchPoint());
    QVERIFY(physicalClick(at(50, 30)));
    QVERIFY(physicalClick(at(55, 30)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    QVERIFY(std::holds_alternative<SketchCircle>(sketch()->entities()[0].geometry));
    // Selecionar volta a selecionar entidades.
    tool("actionFerramentaSelecionar");
    QVERIFY(physicalClick(at(55, 30), QPointF(3, 3))); // sobre a circunferência (R 5)
    QCOMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-1"));
}

void TestE561Ui::cliqueDireitoNaoInterfere()
{
    tool("actionFerramentaLinha");
    QVERIFY(physicalClick(at(10, 10)));
    // Clique direito não cria ponto nem descarta o primeiro ponto.
    QVERIFY(physicalClick(at(30, 30), QPointF(), Qt::RightButton));
    QVERIFY(viewer()->hasPendingSketchPoint());
    QVERIFY(sketch()->entities().empty());
    QVERIFY(physicalClick(at(40, 10)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    const auto& l = std::get<SketchLine>(sketch()->entities()[0].geometry);
    QVERIFY(approx(l.x1, 10) && approx(l.x2, 40));
}

void TestE561Ui::focoAoSelecionarFerramenta()
{
    // Foco em outro widget (árvore); selecionar a ferramenta devolve o foco ao visualizador:
    // o primeiro clique já é o primeiro ponto e Esc funciona sem clique extra.
    m_window->operationsTree()->setFocus();
    QTRY_VERIFY(m_window->operationsTree()->hasFocus());
    tool("actionFerramentaLinha");
    QTRY_VERIFY(viewer()->hasFocus());
    QVERIFY(physicalClick(at(10, 10)));
    QVERIFY(viewer()->hasPendingSketchPoint());
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    QVERIFY(!viewer()->hasPendingSketchPoint());
}

void TestE561Ui::filletEChamfer2DComTremido()
{
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    tool("actionFerramentaFillet2D");
    m_window->nextCornerValue = 10.0;
    QVERIFY(physicalClick(at(99.6, 59.7), QPointF(5, -4)));
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(sketch()->entities().size(), size_t(2));
    QVERIFY(std::holds_alternative<SketchFillet>(sketch()->entities()[1].geometry));
    tool("actionFerramentaChamfer2D");
    m_window->nextCornerValue = 5.0;
    QVERIFY(physicalClick(at(0.3, 0.2), QPointF(-6, 2)));
    QCOMPARE(sketch()->entities().size(), size_t(3));
    QVERIFY(std::holds_alternative<SketchChamfer>(sketch()->entities()[2].geometry));
    // Arrasto não aplica acabamento (evita canto escolhido por engano).
    QVERIFY(drag(viewer(), at(100, 0), at(70, 30), Qt::LeftButton));
    QCOMPARE(sketch()->entities().size(), size_t(3));
}

void TestE561Ui::esbocoSobreFaceEExtrusao()
{
    // Retângulo do bloco desenhado com cliques físicos, extrusão de novo corpo.
    tool("actionFerramentaRetangulo");
    QVERIFY(physicalClick(at(0, 0), QPointF(2, 2)));
    QVERIFY(physicalClick(at(100, 60), QPointF(-3, 1)));
    QVERIFY(m_window->finishSketch());
    const auto& rect = std::get<SketchRectangle>(m_window->document().sketches()[0].entities()[0].geometry);
    QVERIFY(approx(rect.width, 100) && approx(rect.height, 60));
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0}));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    // Face superior selecionada pelo clique físico (com tremido) e esboço sobre ela.
    QVERIFY(physicalClick(at3d(50, 30, 20), QPointF(3, 2)));
    QTRY_COMPARE(viewer()->selectedShapes().size(), 1);
    action("actionEsbocoNaFace")->trigger();
    QVERIFY(m_window->isEditingSketch());
    QVERIFY(sketch()->plane().type() == PlaneType::Face);
    tool("actionFerramentaCirculo");
    QVERIFY(physicalClick(at(50, 30), QPointF(4, -3)));
    QVERIFY(hover(at(50, 30), at(58, 30)));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->sketchPreviewPresentation()));
    QVERIFY(physicalClick(at(60, 30)));
    QCOMPARE(sketch()->entities().size(), size_t(1));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E561_02_circulo_na_face");
    QVERIFY(m_window->finishSketch());
    // Perfil fechado extrudado como corte passante.
    ExtrudeParameters cut;
    cut.sketchId = "sk-2";
    cut.mode = ExtrudeMode::Cut;
    cut.targetId = "op-1";
    cut.endCondition = EndCondition::ThroughAll;
    QVERIFY2(m_window->addExtrusion(cut), qPrintable(m_window->errors.join('\n')));
    const double expected = 100.0 * 60.0 * 20.0 - kPi * 100.0 * 20.0;
    qInfo("Volume: %.4f (esperado %.4f)", ShapeInspector::volume(m_window->part()), expected);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - expected) < 0.5);
}

void TestE561Ui::persistenciaDasEntidades()
{
    tool("actionFerramentaLinha");
    QVERIFY(physicalClick(at(0, 0), QPointF(5, 5)));
    QVERIFY(physicalClick(at(30, 0)));
    tool("actionFerramentaArco");
    QVERIFY(physicalClick(at(50, 0)));
    QVERIFY(physicalClick(at(60, 0)));
    QVERIFY(physicalClick(at(50, 10)));
    QVERIFY(m_window->finishSketch());
    const QString path = m_dir->filePath("e561.cadmec");
    QVERIFY(m_window->saveDocumentTo(path));
    QVERIFY(m_window->openDocumentFrom(path));
    const auto& sketches = m_window->document().sketches();
    QCOMPARE(sketches.size(), size_t(1));
    QCOMPARE(sketches[0].entities().size(), size_t(2));
    QVERIFY(std::holds_alternative<SketchLine>(sketches[0].entities()[0].geometry));
    const auto& a = std::get<SketchArc>(sketches[0].entities()[1].geometry);
    QVERIFY(approx(a.radius, 10));
}

QTEST_MAIN(TestE561Ui)
#include "test_e561_ui.moc"
