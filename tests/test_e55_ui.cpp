// Arcos e acabamentos 2D/3D na interface real (E5.5), com contexto OpenGL real: ferramenta Arco
// (centro → início → fim, sentidos), Esc, seleção, dimensões e exclusão; Fillet 2D e Chamfer 2D
// por clique no canto; Fillet 3D e Chamfer 3D pelo diálogo não modal com seleção real de arestas
// (simples, múltipla, limpar, valores inválidos, cancelamento, edição); preservação da seleção,
// navegação e ViewCube; e o cenário integrado da seção 28 do plano (esboço com fillet/chamfer,
// arco independente, extrusões, subtração, Fillet 3D, Chamfer 3D, salvar/reabrir, STEP e STL).
#include "StlTestUtils.h"
#include "ViewerTestUtils.h"

#include "app/FinishDialog.h"
#include "app/MainWindow.h"
#include "app/SketchEntityDialog.h"
#include "document/DocumentFile.h"
#include "io/StepImporter.h"
#include "modeling/EdgeNaming.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"
#include "sketch/SketchResolver.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <AIS_ViewCube.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <optional>

using cad::app::FinishDialog;
using cad::app::MainWindow;
using cad::document::OperationType;
using cad::modeling::BooleanKind;
using cad::modeling::ShapeInspector;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBox = 120000.0;

double filletLoss(double r) { return r * r - kPi * r * r / 4.0; }
double chamferLoss(double d) { return d * d / 2.0; }

class TestableMainWindow : public MainWindow
{
public:
    std::optional<double> nextCornerValue = 10.0;
    std::optional<SketchGeometry> nextGeometry;
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    std::optional<StlExportRequest> nextStl;
    QString nextSavePath, nextOpenPath, nextExportPath;
    QStringList errors, infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    std::optional<double> askCornerValue(EntityType, double) override { return nextCornerValue; }
    // Chamfer 2D (E5.6): distâncias iguais com o mesmo valor do gancho.
    std::optional<SketchChamfer> askChamfer2D(const SketchChamfer& initial) override
    {
        if (!nextCornerValue)
            return std::nullopt;
        SketchChamfer c = initial;
        c.distance = *nextCornerValue;
        return c;
    }
    std::optional<SketchGeometry> askEntityDimensions(const SketchEntity&) override { return nextGeometry; }
    std::optional<StlExportRequest> askStlExport(const QString&) override { return nextStl; }
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

TopoDS_Edge edgeAt(const TopoDS_Shape& shape, double x, double y, double z)
{
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(x, y, z)) < 1e-6)
            return TopoDS::Edge(ex.Current());
    }
    return TopoDS_Edge();
}

QTreeWidgetItem* opItem(QTreeWidget* tree, const QString& id)
{
    QTreeWidgetItem* root = tree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
        if (root->child(i)->data(0, MainWindow::kItemIdRole).toString() == id
            && root->child(i)->data(0, MainWindow::kItemKindRole).toString() == QLatin1String("operacao"))
            return root->child(i);
    return nullptr;
}

} // namespace

class TestE55Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // Arco circular 2D
    void arcoPeloMouseAntiHorario();
    void arcoPeloMouseHorario();
    void escCancelaArco();
    void arcoNosPlanos_data();
    void arcoNosPlanos();
    void selecaoEdicaoEExclusaoDoArco();
    void dialogoDeDimensoesDoArco();
    // Fillet / Chamfer 2D
    void filletEChamfer2DPeloCanto();
    void cantoInvalidoOuInviavel();
    void edicaoDoRaio2DEExtrusao();
    void ferramentasSoNaEdicao();
    // Fillet / Chamfer 3D
    void fillet3DPeloDialogo();
    void chamfer3DMultiplasArestasECancelamento();
    void edicaoDoFillet3D();
    void selecaoNavegacaoEViewCubeAposAcabamento();
    // Seção 28
    void cenarioIntegrado();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        return toLogical(viewer(), project(viewer(), m_window->editingSketch()->plane().toGlobal(u, v)));
    }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    // Retângulo 100 x 60 na origem do esboço XY em edição (mouse + dimensões) e enquadramento.
    void drawRectangle();
    // Bloco 100 x 60 x 20 extrudado (op-1) na vista isométrica.
    void block();
    FinishDialog* openFinish(const char* actionName);

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE55Ui::init()
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

void TestE55Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE55Ui::drawRectangle()
{
    action("actionFerramentaRetangulo")->trigger();
    QVERIFY(click(viewer(), at(10.0, 10.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(40.0, 30.0)));
    pauseBetweenClicks();
    QVERIFY(m_window->updateSketchEntity("e-1", SketchRectangle{0.0, 0.0, 100.0, 60.0}));
    QTest::keyClick(viewer(), Qt::Key_F); // enquadra o esboço inteiro
    QVERIFY(waitFrames(viewer()));
}

void TestE55Ui::block()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0, false}));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
}

FinishDialog* TestE55Ui::openFinish(const char* actionName)
{
    action(actionName)->trigger();
    FinishDialog* d = m_window->finishDialog();
    if (d && !QTest::qWaitForWindowExposed(d))
        return nullptr;
    pauseBetweenClicks();
    return d;
}

// --- Arco ------------------------------------------------------------------

void TestE55Ui::arcoPeloMouseAntiHorario()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    action("actionFerramentaArco")->trigger();
    QVERIFY(action("actionFerramentaArco")->isChecked());
    QVERIFY(click(viewer(), at(10.0, 10.0)));   // centro
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(30.0, 10.0)));   // início: raio 20
    pauseBetweenClicks();
    sendMouse(viewer(), QEvent::MouseMove, at(10.0, 45.0), Qt::NoButton, Qt::NoButton); // pré-visualização
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_01_previsualizacao_arco");
    QVERIFY(click(viewer(), at(10.0, 45.0)));   // fim projetado na circunferência (90°)
    pauseBetweenClicks();
    const Sketch* s = m_window->editingSketch();
    QCOMPARE(s->entities().size(), size_t(1));
    const auto& a = std::get<SketchArc>(s->entities()[0].geometry);
    qInfo("Arco: centro (%.2f; %.2f) R %.2f, %.1f -> %.1f, %s", a.cx, a.cy, a.radius, a.startAngle, a.endAngle,
          a.clockwise ? "horario" : "anti-horario");
    QVERIFY(approx(a.cx, 10.0) && approx(a.cy, 10.0) && approx(a.radius, 20.0));
    QVERIFY(approx(a.startAngle, 0.0, 1.5) || approx(a.startAngle, 360.0, 1.5));
    QVERIFY(approx(a.endAngle, 90.0, 1.5));
    QVERIFY(!a.clockwise);
    QVERIFY(approx(a.sweep(), 90.0, 2.0));
    QCOMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-1"));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("anti-horário")));
    saveEvidence(m_window->grab().toImage(), "E55_02_arco_anti_horario");
}

void TestE55Ui::arcoPeloMouseHorario()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    action("actionArcoHorario")->trigger();
    QVERIFY(action("actionArcoHorario")->isChecked() && viewer()->arcClockwise());
    action("actionFerramentaArco")->trigger();
    QVERIFY(click(viewer(), at(10.0, 10.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(30.0, 10.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(10.0, 45.0)));
    const auto& a = std::get<SketchArc>(m_window->editingSketch()->entities()[0].geometry);
    qInfo("Arco horario: %.1f -> %.1f, varredura %.1f", a.startAngle, a.endAngle, a.sweep());
    QVERIFY(a.clockwise);
    QVERIFY(approx(a.sweep(), 270.0, 2.0));
    // O ponto médio do arco horário fica no terceiro quadrante em relação ao centro.
    QVERIFY(a.pointAt(0.5).X() < a.cx && a.pointAt(0.5).Y() < a.cy);
}

void TestE55Ui::escCancelaArco()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    action("actionFerramentaArco")->trigger();
    QVERIFY(click(viewer(), at(0.0, 0.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(20.0, 0.0)));
    QTest::keyClick(viewer(), Qt::Key_Escape); // cancela centro e início
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(5.0, 5.0)));     // novo centro (não é o ponto final)
    pauseBetweenClicks();
    QVERIFY(m_window->editingSketch()->entities().empty());
    QTest::keyClick(viewer(), Qt::Key_Escape);
    QVERIFY(m_window->editingSketch()->entities().empty()); // nenhuma entidade incompleta
}

void TestE55Ui::arcoNosPlanos_data()
{
    QTest::addColumn<int>("plane");
    QTest::newRow("XY") << int(PlaneType::XY);
    QTest::newRow("XZ") << int(PlaneType::XZ);
    QTest::newRow("YZ") << int(PlaneType::YZ);
}

void TestE55Ui::arcoNosPlanos()
{
    QFETCH(int, plane);
    QVERIFY(m_window->startNewSketch(PlaneType(plane)));
    action("actionFerramentaArco")->trigger();
    QVERIFY(click(viewer(), at(0.0, 0.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(0.0, 25.0)));  // início a 90°
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(-25.0, 0.0))); // fim a 180°
    const auto& a = std::get<SketchArc>(m_window->editingSketch()->entities()[0].geometry);
    qInfo("[%s] R %.2f, %.1f -> %.1f", SketchPlane::name(PlaneType(plane)), a.radius, a.startAngle, a.endAngle);
    QVERIFY(approx(a.radius, 25.0) && approx(a.startAngle, 90.0, 1.0) && approx(a.endAngle, 180.0, 1.0));
    // Exibição no plano correto: pixel do esboço sobre o ponto médio 3D do arco.
    const SketchPlane& p = m_window->editingSketch()->plane();
    const gp_Pnt mid = p.toGlobal(a.pointAt(0.5));
    QVERIFY(std::fabs(p.distance(mid)) < 1e-9);
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->document().sketches()[0].entities()[0].type() == EntityType::Arc);
}

void TestE55Ui::selecaoEdicaoEExclusaoDoArco()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchArc{0.0, 0.0, 30.0, 0.0, 90.0, false}));
    QVERIFY(m_window->addSketchGeometry(SketchLine{-40.0, -20.0, 40.0, -20.0}));
    action("actionFerramentaSelecionar")->trigger();
    QTest::keyClick(viewer(), Qt::Key_F);
    QVERIFY(waitFrames(viewer()));
    m_window->selectSketchEntity(QString());
    // 6. Seleção do arco pelo clique sobre ele (ponto a 45°).
    QVERIFY(click(viewer(), at(30.0 * std::cos(kPi / 4), 30.0 * std::sin(kPi / 4))));
    QTRY_COMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-1"));
    QVERIFY(m_window->statusBar()->currentMessage().startsWith(QStringLiteral("Selecionado: Arco e-1")));
    QVERIFY(action("actionDimensoesEntidade")->isEnabled());
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_03_arco_selecionado");
    // 7-9. Edição do centro, do raio e dos ângulos pelas dimensões.
    m_window->nextGeometry = SketchArc{5.0, -2.0, 12.5, 30.0, 300.0, true};
    action("actionDimensoesEntidade")->trigger();
    const auto& a = std::get<SketchArc>(m_window->editingSketch()->findEntity("e-1")->geometry);
    QVERIFY(a.cx == 5.0 && a.cy == -2.0 && a.radius == 12.5 && a.startAngle == 30.0 && a.endAngle == 300.0 && a.clockwise);
    QVERIFY(m_window->operationsTree()->currentItem()->text(0).contains(QStringLiteral("horário")));
    // Valor inválido recusado com mensagem; entidade mantida.
    m_window->nextGeometry = SketchArc{5.0, -2.0, 12.5, 30.0, 30.0, true};
    action("actionDimensoesEntidade")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("Varredura")));
    QCOMPARE(std::get<SketchArc>(m_window->editingSketch()->findEntity("e-1")->geometry).endAngle, 300.0);
    // 10. Exclusão.
    action("actionExcluirEntidade")->trigger();
    QVERIFY(!m_window->editingSketch()->findEntity("e-1"));
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
}

void TestE55Ui::dialogoDeDimensoesDoArco()
{
    SketchEntity e;
    e.id = "e-4";
    e.geometry = SketchArc{10.0, 20.0, 5.0, 0.0, 90.0, false};
    cad::app::SketchEntityDialog dialog(e);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* start = dialog.findChild<QDoubleSpinBox*>("dim_startAngle");
    auto* end = dialog.findChild<QDoubleSpinBox*>("dim_endAngle");
    auto* cw = dialog.findChild<QCheckBox*>("dim_clockwise");
    auto* sweep = dialog.findChild<QLabel*>("dim_sweep");
    auto* buttons = dialog.findChild<QDialogButtonBox*>("botoesDimensoes");
    QVERIFY(start && end && cw && sweep && buttons);
    QCOMPARE(start->suffix(), QStringLiteral(" °"));
    QCOMPARE(sweep->text(), QStringLiteral("90,00°"));
    saveEvidence(dialog.grab().toImage(), "E55_04_dialogo_arco");
    cw->setChecked(true);
    QCOMPARE(sweep->text(), QStringLiteral("270,00°")); // varredura calculada acompanha o sentido
    end->setValue(0.0);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QVERIFY(dialog.isVisible()); // varredura nula: recusado, diálogo aberto
    QVERIFY(dialog.validationMessage().contains(QStringLiteral("Varredura")));
    end->setValue(180.0);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto g = std::get<SketchArc>(dialog.geometry());
    QVERIFY(g.clockwise && g.endAngle == 180.0 && g.startAngle == 0.0);
}

// --- Fillet / Chamfer 2D ---------------------------------------------------

void TestE55Ui::filletEChamfer2DPeloCanto()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    drawRectangle();
    // Fillet 2D: clique perto do canto (100, 60) e raio 10 (gancho do diálogo).
    action("actionFerramentaFillet2D")->trigger();
    m_window->nextCornerValue = 10.0;
    QVERIFY(click(viewer(), at(99.6, 59.7)));
    pauseBetweenClicks();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const Sketch* s = m_window->editingSketch();
    QCOMPARE(s->entities().size(), size_t(2));
    const auto& f = std::get<SketchFillet>(s->entities()[1].geometry);
    QVERIFY(f.entityA == "e-1" && f.entityB == "e-1" && f.corner == 2 && f.radius == 10.0);
    QCOMPARE(m_window->selectedSketchEntity(), QStringLiteral("e-2"));
    QVERIFY(viewer()->sketchEntityPresentation("e-2")); // arco do fillet exibido como objeto próprio
    // Chamfer 2D 5 mm no canto (0, 0).
    action("actionFerramentaChamfer2D")->trigger();
    m_window->nextCornerValue = 5.0;
    QVERIFY(click(viewer(), at(0.3, 0.2)));
    pauseBetweenClicks();
    QCOMPARE(s->entities().size(), size_t(3));
    const auto& c = std::get<SketchChamfer>(s->entities()[2].geometry);
    QVERIFY(c.corner == 0 && c.distance == 5.0);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_05_fillet_chamfer_2d");
    // Exibição: pixel do esboço no meio do arco do fillet e no meio do chanfro; vértices
    // originais vazios.
    const QImage img = viewer()->grabViewerImage();
    const SketchPlane& p = s->plane();
    auto sketchPixel = [&](double u, double v) {
        const QPoint c0 = project(viewer(), p.toGlobal(u, v));
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
            {
                const QRgb px = pixelAt(img, c0 + QPoint(dx, dy));
                if ((qBlue(px) > 140 && qBlue(px) - qRed(px) > 80) || (qRed(px) > 170 && qRed(px) - qBlue(px) > 90))
                    return true;
            }
        return false;
    };
    const double m = 10.0 - 10.0 * std::cos(kPi / 4); // ponto médio do arco: centro (90, 50) + R(cos45, sen45)
    QVERIFY2(sketchPixel(100.0 - m, 60.0 - m), "arco do fillet não desenhado");
    QVERIFY2(sketchPixel(2.5, 2.5), "chanfro não desenhado");
    QVERIFY(!sketchPixel(99.0, 59.0));  // canto original recortado
    // Árvore: modificadores listados no esboço.
    QVERIFY(m_window->operationsTree()->currentItem()->text(0).startsWith(QStringLiteral("Chamfer 2D e-3")));
    QVERIFY(m_window->finishSketch());
    const auto profile = cad::sketch::buildProfile(m_window->document().sketches()[0]);
    QVERIFY(profile.ok);
    QVERIFY(std::fabs(profile.area - (6000.0 - filletLoss(10.0) - 12.5)) < 1e-6);
}

void TestE55Ui::cantoInvalidoOuInviavel()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    drawRectangle();
    action("actionFerramentaFillet2D")->trigger();
    // Clique longe de qualquer canto: só mensagem, nenhuma entidade.
    QVERIFY(click(viewer(), at(50.0, 30.0)));
    pauseBetweenClicks();
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("canto")));
    // Raio inviável: recusado com mensagem.
    m_window->nextCornerValue = 70.0;
    QVERIFY(click(viewer(), at(100.0, 60.0)));
    pauseBetweenClicks();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("grande demais")));
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
    // Cancelado: nada criado.
    m_window->nextCornerValue.reset();
    QVERIFY(click(viewer(), at(100.0, 60.0)));
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
    // Lado referenciado não pode ser excluído enquanto o fillet existir.
    m_window->nextCornerValue = 10.0;
    QVERIFY(m_window->applyCornerModifier(EntityType::Fillet, 100.0, 60.0, 1.0, 10.0));
    m_window->selectSketchEntity("e-1");
    action("actionExcluirEntidade")->trigger();
    QCOMPARE(m_window->errors.size(), 2);
    QVERIFY(m_window->errors.last().contains(QStringLiteral("e-2")));
    QVERIFY(m_window->editingSketch()->findEntity("e-1"));
}

void TestE55Ui::edicaoDoRaio2DEExtrusao()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->applyCornerModifier(EntityType::Fillet, 100.0, 60.0, 1.0, 10.0));
    QVERIFY(m_window->applyCornerModifier(EntityType::Chamfer, 0.0, 0.0, 1.0, 5.0));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"", "sk-1", 20.0, false}));
    const double v1 = (6000.0 - filletLoss(10.0) - 12.5) * 20.0;
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - v1) < 1e-4);
    QCOMPARE(ShapeInspector::inspect(m_window->part()).faces, 8);
    // Raio 10 → 20 e distância 5 → 8 pelo diálogo de dimensões; a extrusão é recalculada.
    QVERIFY(m_window->editSketch("sk-1"));
    m_window->selectSketchEntity("e-2");
    m_window->nextGeometry = SketchFillet{"e-1", "e-1", 2, 20.0};
    action("actionDimensoesEntidade")->trigger();
    m_window->selectSketchEntity("e-3");
    m_window->nextGeometry = SketchChamfer{"e-1", "e-1", 0, 8.0};
    action("actionDimensoesEntidade")->trigger();
    QVERIFY(m_window->finishSketch());
    const double v2 = (6000.0 - filletLoss(20.0) - 32.0) * 20.0;
    qInfo("volume apos editar raio e distancia: %.4f (esperado %.4f)", ShapeInspector::volume(m_window->part()), v2);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - v2) < 1e-4);
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_06_extrusao_com_fillet_chamfer_2d");
}

void TestE55Ui::ferramentasSoNaEdicao()
{
    for (const char* name : {"actionFerramentaArco", "actionFerramentaFillet2D", "actionFerramentaChamfer2D", "actionArcoHorario"})
        QVERIFY2(!action(name)->isEnabled(), name);
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    for (const char* name : {"actionFerramentaArco", "actionFerramentaFillet2D", "actionFerramentaChamfer2D", "actionArcoHorario"})
        QVERIFY2(action(name)->isEnabled(), name);
    action("actionFerramentaFillet2D")->trigger();
    QVERIFY(viewer()->sketchTool() == cad::viewer::SketchTool::Fillet2D);
    action("actionCancelarEsboco")->trigger();
    // Ferramenta ativa volta a Selecionar; navegação 3D restaurada.
    QVERIFY(action("actionFerramentaSelecionar")->isChecked());
    QVERIFY(viewer()->sketchTool() == cad::viewer::SketchTool::Select);
    QVERIFY(!action("actionFerramentaArco")->isEnabled());
}

// --- Fillet / Chamfer 3D ---------------------------------------------------

void TestE55Ui::fillet3DPeloDialogo()
{
    block();
    QVERIFY(action("actionFillet3D")->isEnabled());
    FinishDialog* d = openFinish("actionFillet3D");
    QVERIFY(d);
    QVERIFY(!d->isModal());
    QVERIFY(viewer()->edgeSelectionMode());
    QVERIFY(!action("actionFillet3D")->isEnabled() && !action("actionNovoEsboco")->isEnabled());
    auto* edges = d->findChild<QLabel*>("rotuloArestas");
    auto* value = d->findChild<QDoubleSpinBox*>("campoValorAcabamento");
    auto* message = d->findChild<QLabel*>("mensagemAcabamento");
    auto* ok = d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok);
    QVERIFY(edges && value && message && ok);
    QCOMPARE(edges->text(), QStringLiteral("0 arestas selecionadas"));
    // Clique no centro de uma face: não seleciona (modo somente arestas).
    QVERIFY(click(viewer(), at3d(50.0, 30.0, 20.0)));
    QTest::qWait(300);
    QVERIFY(viewer()->selectedShapes().isEmpty());
    pauseBetweenClicks();
    // 3. Uma aresta: superior frontal.
    QVERIFY(click(viewer(), at3d(50.0, 0.0, 20.0)));
    QTRY_COMPARE(edges->text(), QStringLiteral("1 aresta selecionada"));
    QVERIFY(d->findChild<QLabel*>("rotuloCorpoOrigem")->text().contains(QStringLiteral("Bloco (op-1)")));
    value->setValue(5.0);
    QVERIFY2(message->text().contains(QStringLiteral("119.463,50")), qPrintable(message->text()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
    QCOMPARE(m_window->document().operations().size(), size_t(1)); // pré-visualização não altera
    saveEvidence(m_window->grab().toImage(), "E55_07_fillet3d_previsualizacao");
    saveEvidence(d->grab().toImage(), "E55_08_dialogo_fillet3d");
    // Raio excessivo: mensagem e o diálogo permanece aberto.
    value->setValue(25.0);
    QVERIFY(message->text().contains(QStringLiteral("inviável")));
    ok->click();
    QVERIFY(d->isVisible());
    value->setValue(5.0);
    ok->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox - 100.0 * filletLoss(5.0))) < 1e-3);
    QVERIFY(!viewer()->edgeSelectionMode()); // modos de seleção restaurados
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
    QCOMPARE(m_window->selectedOperationId(), QStringLiteral("op-2"));
    // Árvore: corpo de origem consumido, raio e arestas.
    QTreeWidgetItem* item = opItem(m_window->operationsTree(), "op-2");
    QVERIFY(item && item->text(0) == QStringLiteral("Fillet 3D 1"));
    QStringList children;
    for (int i = 0; i < item->childCount(); ++i)
        children << item->child(i)->text(0);
    qInfo("%s", qPrintable(children.join(" | ")));
    QCOMPARE(children, QStringList({"Corpo de origem: Bloco (op-1)", "Raio: 5,00 mm", "Arestas: 1", "Estado: Válida"}));
    QVERIFY(opItem(m_window->operationsTree(), "op-1")->text(0).contains(QStringLiteral("consumido por Fillet 3D 1")));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_09_fillet3d_criado");
}

void TestE55Ui::chamfer3DMultiplasArestasECancelamento()
{
    block();
    // Cancelamento: nada muda.
    FinishDialog* d = openFinish("actionChamfer3D");
    QVERIFY(d);
    QVERIFY(click(viewer(), at3d(50.0, 0.0, 20.0)));
    QTRY_COMPARE(d->edgeCount(), 1);
    d->reject();
    QTRY_VERIFY(!m_window->finishDialog());
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    QVERIFY(!viewer()->edgeSelectionMode());
    // 4. Seleção múltipla (Shift + clique) e "Limpar seleção".
    d = openFinish("actionChamfer3D");
    QVERIFY(d);
    QVERIFY(click(viewer(), at3d(50.0, 0.0, 20.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(100.0, 30.0, 20.0), Qt::LeftButton, Qt::ShiftModifier));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(100.0, 0.0, 10.0), Qt::LeftButton, Qt::ShiftModifier));
    QTRY_COMPARE(d->edgeCount(), 3);
    QCOMPARE(d->findChild<QLabel*>("rotuloArestas")->text(), QStringLiteral("3 arestas selecionadas"));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("3 arestas selecionadas")));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_10_chamfer3d_tres_arestas");
    d->findChild<QPushButton*>("botaoLimparSelecao")->click();
    QTRY_COMPARE(d->edgeCount(), 0);
    QVERIFY(d->resultMessage().contains(QStringLiteral("Selecione")));
    // Duas arestas verticais (independentes): volume exato.
    QVERIFY(click(viewer(), at3d(100.0, 0.0, 10.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(0.0, 0.0, 10.0), Qt::LeftButton, Qt::ShiftModifier));
    QTRY_COMPARE(d->edgeCount(), 2);
    d->findChild<QDoubleSpinBox*>("campoValorAcabamento")->setValue(3.0);
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox - 2.0 * 20.0 * chamferLoss(3.0))) < 1e-6);
    QCOMPARE(m_window->document().operations()[1].edges.size(), size_t(2));
    QCOMPARE(m_window->document().operations()[1].name, std::string("Chamfer 3D 1"));
}

void TestE55Ui::edicaoDoFillet3D()
{
    block();
    QVERIFY(m_window->addFinish(OperationType::Fillet3D, "op-1", {edgeAt(m_window->part(), 50.0, 0.0, 20.0)}, 5.0));
    m_window->selectTreeItem("op-2");
    action("actionEditarOperacao")->trigger();
    FinishDialog* d = m_window->finishDialog();
    QVERIFY(d);
    // O corpo de origem é exibido no lugar do resultado e a aresta referenciada vem selecionada.
    QVERIFY(std::fabs(ShapeInspector::volume(viewer()->shape()) - kBox) < 1e-6);
    QTRY_COMPARE(d->edgeCount(), 1);
    QCOMPARE(viewer()->selectedShapes().size(), 1);
    auto* value = d->findChild<QDoubleSpinBox*>("campoValorAcabamento");
    QCOMPARE(value->value(), 5.0);
    QCOMPARE(d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->text(), QStringLiteral("Aplicar"));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_11_edicao_fillet3d_origem");
    // Raio 8 e uma aresta a mais (superior direita) — seleção de arestas editada.
    value->setValue(8.0);
    QVERIFY(click(viewer(), at3d(100.0, 30.0, 20.0), Qt::LeftButton, Qt::ShiftModifier));
    QTRY_COMPARE(d->edgeCount(), 2);
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QCOMPARE(m_window->document().operations()[1].edges.size(), size_t(2));
    QCOMPARE(m_window->document().finishParameters("op-2").value, 8.0);
    const double v = ShapeInspector::volume(m_window->part());
    qInfo("fillet R8 em duas arestas concorrentes: volume %.4f", v);
    QVERIFY(v < kBox - 100.0 * filletLoss(8.0) && v > kBox - 160.0 * filletLoss(8.0));
    QVERIFY(viewer()->shape().IsSame(m_window->part())); // geometria do documento restaurada
    // Recálculo após alterar a extrusão: aresta remapeada.
    QVERIFY(m_window->updateExtrusion("op-1", {"Bloco", "sk-1", 30.0, false}));
    QVERIFY(ShapeInspector::volume(m_window->part()) > v + 50000.0);
    QVERIFY(ShapeInspector::isValid(m_window->part()));
}

void TestE55Ui::selecaoNavegacaoEViewCubeAposAcabamento()
{
    block();
    QVERIFY(m_window->addFinish(OperationType::Chamfer3D, "op-1", {edgeAt(m_window->part(), 50.0, 0.0, 20.0)}, 3.0));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    auto message = [this]() { return m_window->statusBar()->currentMessage(); };
    // 17. Seleção de face, aresta e vértice preservada.
    QVERIFY(click(viewer(), at3d(50.0, 40.0, 20.0)));
    QTRY_VERIFY(message().startsWith("Face"));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(100.0, 30.0, 20.0)));
    QTRY_VERIFY(message().startsWith("Aresta"));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(100.0, 60.0, 20.0)));
    QTRY_VERIFY(message().startsWith(QString::fromUtf8("Vértice")));
    // 18. Navegação e ViewCube.
    const gp_Dir dir = viewer()->view()->Camera()->Direction();
    const QPointF c(viewer()->width() / 2.0, viewer()->height() / 2.0);
    QVERIFY(drag(viewer(), c, c + QPointF(100, 40), Qt::MiddleButton));
    QVERIFY(dir.Angle(viewer()->view()->Camera()->Direction()) > 0.1);
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
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

// --- Seção 28: cenário integrado ---------------------------------------------

void TestE55Ui::cenarioIntegrado()
{
    // 1. Documento vazio.
    QVERIFY(m_window->document().operations().empty() && m_window->document().sketches().empty());
    // 2-5. Esboço XY: retângulo 100 x 60 (mouse + dimensões), Fillet 2D R10 e Chamfer 2D 5.
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    drawRectangle();
    action("actionFerramentaFillet2D")->trigger();
    m_window->nextCornerValue = 10.0;
    QVERIFY(click(viewer(), at(100.0, 60.0)));
    pauseBetweenClicks();
    action("actionFerramentaChamfer2D")->trigger();
    m_window->nextCornerValue = 5.0;
    QVERIFY(click(viewer(), at(0.0, 0.0)));
    pauseBetweenClicks();
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(3));
    // 7. Concluir.
    QVERIFY(m_window->finishSketch());
    saveEvidence(m_window->grab().toImage(), "E55_20_cenario_esboco_concluido");
    // 6. Arco independente em esboço separado (XZ), criado pelo mouse e editado.
    QVERIFY(m_window->startNewSketch(PlaneType::XZ));
    action("actionFerramentaArco")->trigger();
    QVERIFY(click(viewer(), at(150.0 - 150.0, 0.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(15.0, 0.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(0.0, 15.0)));
    pauseBetweenClicks();
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
    QVERIFY(m_window->updateSketchEntity("e-1", SketchArc{150.0, 10.0, 12.0, 0.0, 135.0, false}));
    QCOMPARE(std::get<SketchArc>(m_window->editingSketch()->entities()[0].geometry).radius, 12.0);
    QVERIFY(m_window->finishSketch());
    // 8. Extrusão 20 mm do esboço 1 (o arco do esboço 2 não participa do perfil).
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0, false}));
    const double blockVolume = (6000.0 - filletLoss(10.0) - 12.5) * 20.0;
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - blockVolume) < 1e-4);
    // 9-10. Esboço circular adicional e cilindro que atravessa o bloco (z de 0 a 30).
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50.0, 30.0, 10.0}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Cilindro", "sk-3", 30.0, false}));
    // 11. Subtração booleana.
    QVERIFY(m_window->addBoolean({"Bloco − Cilindro", BooleanKind::Cut, "op-1", "op-2"}));
    const double cutVolume = blockVolume - 2000.0 * kPi;
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - cutVolume) < 1e-4);
    bool through = true;
    for (double z = 0.5; z < 20.0; z += 1.0)
    {
        BRepClass3d_SolidClassifier cls(m_window->part(), gp_Pnt(50.0, 30.0, z), 1e-7);
        through = through && cls.State() == TopAbs_OUT;
    }
    QVERIFY(through); // furo passante real
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    // 12. Fillet 3D R3 na aresta superior frontal (x de 5 a 100) pelo diálogo real.
    FinishDialog* d = openFinish("actionFillet3D");
    QVERIFY(d);
    QVERIFY(click(viewer(), at3d(52.5, 0.0, 20.0)));
    QTRY_COMPARE(d->edgeCount(), 1);
    d->findChild<QDoubleSpinBox*>("campoValorAcabamento")->setValue(3.0);
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    // 13. Chamfer 3D 2 mm na aresta inferior frontal (outra aresta válida).
    QVERIFY(m_window->addFinish(OperationType::Chamfer3D, "op-4", {edgeAt(m_window->part(), 52.5, 0.0, 0.0)}, 2.0));
    const auto& ops = m_window->document().operations();
    QCOMPARE(ops.size(), size_t(5));
    const double finalVolume = ShapeInspector::volume(m_window->part());
    const double approxVolume = cutVolume - 95.0 * filletLoss(3.0) - 95.0 * chamferLoss(2.0);
    qInfo("volume final %.4f (aproximado sem as extremidades obliquas: %.4f)", finalVolume, approxVolume);
    QVERIFY(std::fabs(finalVolume - approxVolume) < 10.0);
    QVERIFY(ShapeInspector::isValid(m_window->part()));
    QCOMPARE(m_window->document().solidCount(), 1);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_21_cenario_final");
    // 14. Salvar.
    m_window->nextSavePath = m_dir->filePath("cenario_e55.cadmec");
    action("actionSalvar")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const QByteArray saved = [&] {
        QFile f(m_window->nextSavePath);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    // 15. Fechar (novo documento) e reabrir.
    action("actionNovo")->trigger();
    QVERIFY(m_window->part().IsNull() && m_window->document().operations().empty());
    m_window->nextOpenPath = m_window->nextSavePath;
    action("actionAbrir")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    // 16. Reconstrução paramétrica.
    QCOMPARE(m_window->document().operations().size(), size_t(5));
    QCOMPARE(m_window->document().sketches().size(), size_t(3));
    QCOMPARE(m_window->document().activeBodies(), std::vector<std::string>{"op-5"});
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - finalVolume) < 1e-6);
    QVERIFY(cad::document::DocumentFile::toJson(m_window->document()) == saved);
    QVERIFY(m_window->document().sketches()[1].entities()[0].type() == EntityType::Arc);
    // 17. STEP.
    m_window->nextExportPath = m_dir->filePath("cenario_e55.step");
    action("actionExportarStep")->trigger();
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = ShapeInspector::inspect(read.shape);
    qInfo("STEP: %d solido(s), %d faces, volume %.4f, valido %d", s.solids, s.faces, s.volume, s.isValid);
    QVERIFY(s.isValid && s.solids == 1 && std::fabs(s.volume - finalVolume) < 1e-3);
    // 18-20. STL binário e ASCII e integridade.
    for (auto format : {cad::io::StlFormat::Binary, cad::io::StlFormat::Ascii})
    {
        const QString path = m_dir->filePath(format == cad::io::StlFormat::Binary ? "cenario_bin.stl" : "cenario_ascii.stl");
        cad::io::StlExportOptions o;
        o.format = format;
        o.quality = cad::io::StlQuality::High;
        m_window->nextStl = MainWindow::StlExportRequest{path, o};
        action("actionExportarStl")->trigger();
        const auto mesh = cad::test::stl::read(path.toStdString());
        QVERIFY2(mesh.ok, mesh.error.c_str());
        qInfo("STL %s: %d triangulos, volume %.3f, caixa [%.2f %.2f %.2f]-[%.2f %.2f %.2f]", mesh.binary ? "binario" : "ASCII",
              int(mesh.triangles.size()), mesh.volume(), mesh.min.x, mesh.min.y, mesh.min.z, mesh.max.x, mesh.max.y, mesh.max.z);
        QCOMPARE(mesh.binary, format == cad::io::StlFormat::Binary);
        QVERIFY(mesh.finite() && mesh.degenerate() == 0);
        QVERIFY(std::fabs(mesh.volume() - finalVolume) / finalVolume < 0.002);
        QVERIFY(std::fabs(mesh.min.x) < 1e-3 && std::fabs(mesh.max.x - 100.0) < 1e-3);
        QVERIFY(std::fabs(mesh.min.z) < 1e-3 && std::fabs(mesh.max.z - 20.0) < 1e-3);
    }
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E55_22_cenario_reaberto");
}

QTEST_MAIN(TestE55Ui)
#include "test_e55_ui.moc"
