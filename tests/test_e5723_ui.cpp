// E5.7.2.3 na interface real, com contexto OpenGL real (plataforma windows):
//  - campo "Critério de inclusão dos cubos": as três opções existentes mantidas (mesmos textos, mesma
//    ordem, padrão inalterado) e a quarta opção "Projeção dos nós sobre o contorno (ajuste
//    geométrico)";
//  - placa 50 × 50 × 1 mm com furo central passante Ø20 criada pelo esboço, lco = 0,5 mm: caso A
//    (cubo totalmente contido) e caso B (projeção) gerados pelo diálogo; resultado com os
//    indicadores da projeção; a visualização (contornos das células) acompanha as posições
//    ajustadas; exportações ASCII e ANSYS APDL pelos botões com as coordenadas projetadas;
//  - trocar o critério exige regenerar antes de exportar; voltar a uma opção antiga restaura a
//    grade cartesiana; o sólido e o documento não são alterados.
#include "ViewerTestUtils.h"

#include "app/DiscretizationDialog.h"
#include "app/MainWindow.h"
#include "discretization/NodeAsciiExporter.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>

#include <cmath>
#include <functional>

using cad::app::DiscretizationDialog;
using cad::app::MainWindow;
using cad::discretization::Discretization;
using cad::discretization::InclusionCriterion;
using cad::discretization::LengthUnit;
using cad::discretization::NodeAsciiExporter;
using cad::document::ExtrudeParameters;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

class TestableMainWindow : public MainWindow
{
public:
    QString nextNodePath;
    QString nextApdlPath;
    QStringList errors, infos;

protected:
    QString askNodeExportPath(const QString&, LengthUnit) override { return nextNodePath; }
    QString askApdlExportPath(const QString&) override { return nextApdlPath; }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    bool confirmOverwrite(const QString&) override { return true; }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
};

template <typename T>
void whenDialogOpens(std::function<void(T*)> body)
{
    auto* timer = new QTimer;
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, [timer, body]() {
        auto* dialog = qobject_cast<T*>(QApplication::activeModalWidget());
        if (!dialog || !dialog->isVisible())
            return;
        timer->stop();
        timer->deleteLater();
        body(dialog);
    });
    timer->start();
}

QStringList readLines(const QString& path)
{
    QFile f(path);
    QStringList lines;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return lines;
    QTextStream in(&f);
    while (!in.atEnd())
        lines << in.readLine();
    return lines;
}

// Primeiro vértice cuja posição difere da grade (nó projetado); 0 se nenhum.
std::int64_t firstMovedVertex(const Discretization& d)
{
    for (std::int64_t id = 1; id <= d.vertexNodeCount(); ++id)
    {
        const auto n = d.node(id);
        const gp_XYZ g = d.grid().vertex(n.i, n.j, n.k);
        if (n.position.X() != g.X() || n.position.Y() != g.Y() || n.position.Z() != g.Z())
            return id;
    }
    return 0;
}

} // namespace

class TestE5723Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void projecaoPeloDialogo();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QString path(const QString& name) const { return m_dir->filePath(name); }
    // Vista superior ampliada na borda do furo (região em torno de (7; 7), cerca de 6 mm de altura).
    bool zoomOnHoleEdge()
    {
        const auto& cam = viewer()->view()->Camera();
        cam->SetEyeAndCenter(gp_Pnt(7.0, 7.0, 200.0), gp_Pnt(7.0, 7.0, 0.5));
        cam->SetUp(gp_Dir(0, 1, 0));
        cam->SetScale(6.0);
        viewer()->view()->Invalidate();
        viewer()->update();
        return waitFrames(viewer(), 2);
    }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE5723Ui::init()
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
}

void TestE5723Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE5723Ui::projecaoPeloDialogo()
{
    // Placa 50 × 50 × 1 mm (extrusão de um retângulo) com furo central passante Ø20 (corte
    // "atravessar tudo" de um círculo no plano XY).
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{-25, -25, 50, 50}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion(ExtrudeParameters{"Placa", m_window->document().sketches().back().id(), 1.0}));
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{0, 0, 10}));
    QVERIFY(m_window->finishSketch());
    ExtrudeParameters hole;
    hole.name = "Furo";
    hole.sketchId = m_window->document().sketches().back().id();
    hole.mode = cad::document::ExtrudeMode::Cut;
    hole.targetId = "op-1";
    hole.endCondition = cad::document::EndCondition::ThroughAll;
    hole.reversed = true; // o corte parte do plano XY (face inferior) em direção a +Z
    QVERIFY2(m_window->addExtrusion(hole), qPrintable(m_window->errors.join('\n')));
    action("actionVistaSuperior")->trigger();
    QVERIFY(waitFrames(viewer()));
    const TopoDS_Shape original = m_window->document().shape();
    const int revision = m_window->document().revision();
    const QString ascii = path("placa_furo_projecao_nodes.txt");
    const QString apdl = path("placa_furo_projecao_PREP7.inp");

    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        auto* lco = d->findChild<QDoubleSpinBox*>("campoLco");
        auto* criterion = d->findChild<QComboBox*>("campoCriterio");
        auto* result = d->findChild<QLabel*>("resultadoDiscretizacao");
        auto* generate = d->findChild<QPushButton*>("botaoGerar");
        auto* exportAscii = d->findChild<QPushButton*>("botaoExportar");
        auto* exportApdl = d->findChild<QPushButton*>("botaoExportarApdl");
        auto* closeButton = d->findChild<QPushButton*>("botaoFechar");
        QVERIFY(lco && criterion && result && generate && exportAscii && exportApdl && closeButton);

        // Opções: as três existentes inalteradas (texto, ordem e padrão) e a quarta nova.
        QCOMPARE(criterion->count(), 4);
        QCOMPARE(criterion->itemText(0), QString("Cubo totalmente contido no sólido"));
        QCOMPARE(criterion->itemText(1), QString("Centro do cubo localizado no material"));
        QCOMPARE(criterion->itemText(2), QString("Cubo com interseção volumétrica positiva com o sólido"));
        QCOMPARE(criterion->itemText(3), QString("Projeção dos nós sobre o contorno (ajuste geométrico)"));
        QCOMPARE(InclusionCriterion(criterion->itemData(3).toInt()), InclusionCriterion::BoundaryProjection);
        QCOMPARE(criterion->currentIndex(), 0);
        QVERIFY(!criterion->itemData(3, Qt::ToolTipRole).toString().isEmpty());

        // Caso A: cubo totalmente contido.
        lco->setValue(0.5);
        generate->click();
        QVERIFY(m_window->discretization());
        const auto caseA = m_window->discretization();
        QVERIFY(!caseA->adjusted());
        QVERIFY(caseA->cubeCount() > 0);
        QVERIFY(!result->text().contains("Nós projetados"));
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(viewer()->grabViewerImage(), "E5723_01_caso_A_totalmente_contido");
        QVERIFY(zoomOnHoleEdge());
        saveEvidence(viewer()->grabViewerImage(), "E5723_01b_caso_A_borda_do_furo");
        action("actionVistaSuperior")->trigger();
        QVERIFY(waitFrames(viewer()));

        // Caso B: projeção. Trocar o critério exige regenerar antes de exportar.
        criterion->setCurrentIndex(3);
        QVERIFY(!exportAscii->isEnabled());
        QVERIFY(!exportApdl->isEnabled());
        generate->click();
        const auto caseB = m_window->discretization();
        QVERIFY(caseB && caseB != caseA);
        QVERIFY(caseB->adjusted());
        const auto& p = caseB->projection();
        QVERIFY(p.projectedNodes > 0);
        QCOMPARE(p.invalidCells, std::int64_t(0));
        QVERIFY(p.deviationAfterMean < p.deviationBeforeMean);
        QVERIFY(exportAscii->isEnabled());
        QVERIFY(exportApdl->isEnabled());
        const QString html = result->text();
        QVERIFY(html.contains("Projeção dos nós sobre o contorno"));
        QVERIFY(html.contains("Nós projetados"));
        QVERIFY(html.contains("Volume das células ajustadas"));
        QVERIFY(html.contains("Não ajustados integralmente"));
        QVERIFY(html.contains("Células inválidas"));
        // Volume das células deformadas (não Nc × lco³) mais próximo do CAD que o caso A.
        const double vCad = caseB->stats().solidVolume;
        QVERIFY(std::fabs(vCad - (2500.0 - 100.0 * 3.14159265358979323846)) < 1e-6);
        QVERIFY(std::fabs(caseB->discretizedVolume() - vCad) < std::fabs(caseA->cubeVolume() - vCad));
        QVERIFY(caseB->discretizedVolume() != caseB->cubeVolume());

        // Visualização: os contornos das células usam as posições ajustadas.
        QVERIFY(viewer()->hasDiscretization());
        const auto segments = viewer()->cubeEdgeSegments();
        QVERIFY(!segments.IsNull() && segments->VertexNumber() > 0);
        int offGrid = 0;
        const double lcoValue = caseB->grid().lco;
        for (int v = 1; v <= segments->VertexNumber(); ++v)
        {
            const gp_Pnt q = segments->Vertice(v);
            const gp_XYZ o = caseB->grid().origin;
            auto onLattice = [&](double c, double c0) {
                const double t = (c - c0) / lcoValue;
                return std::fabs(t - std::round(t)) < 1e-6;
            };
            if (!onLattice(q.X(), o.X()) || !onLattice(q.Y(), o.Y()))
            {
                ++offGrid;
                // Ponto fora da grade: nó projetado sobre o cilindro do furo (r = 10).
                QVERIFY(std::fabs(std::hypot(q.X(), q.Y()) - 10.0) < 0.5);
            }
        }
        QVERIFY(offGrid > 0);
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(viewer()->grabViewerImage(), "E5723_02_caso_B_projecao");
        QVERIFY(zoomOnHoleEdge());
        saveEvidence(viewer()->grabViewerImage(), "E5723_02b_caso_B_borda_do_furo");
        saveEvidence(d->grab().toImage(), "E5723_03_dialogo_resultado_projecao");

        // Exportações com as coordenadas projetadas.
        const std::int64_t moved = firstMovedVertex(*caseB);
        QVERIFY(moved > 0);
        const gp_XYZ mm = caseB->node(moved, LengthUnit::Millimeter).position;
        const gp_XYZ m = caseB->node(moved, LengthUnit::Meter).position;
        m_window->nextNodePath = ascii;
        exportAscii->click();
        QVERIFY2(QFileInfo::exists(ascii), qPrintable(m_window->errors.join('\n')));
        const QStringList asciiLines = readLines(ascii);
        QCOMPARE(asciiLines.size(), int(caseB->totalNodeCount() + 1));
        // Unidade ASCII: a escolhida no diálogo (aceita mm ou m).
        const QString expectedMm = QString("%1 %2 %3 %4")
                                       .arg(moved)
                                       .arg(QString::fromStdString(NodeAsciiExporter::formatCoordinate(mm.X())),
                                            QString::fromStdString(NodeAsciiExporter::formatCoordinate(mm.Y())),
                                            QString::fromStdString(NodeAsciiExporter::formatCoordinate(mm.Z())));
        const QString expectedM = QString("%1 %2 %3 %4")
                                      .arg(moved)
                                      .arg(QString::fromStdString(NodeAsciiExporter::formatCoordinate(m.X())),
                                           QString::fromStdString(NodeAsciiExporter::formatCoordinate(m.Y())),
                                           QString::fromStdString(NodeAsciiExporter::formatCoordinate(m.Z())));
        QVERIFY(asciiLines.contains(expectedMm) || asciiLines.contains(expectedM));

        m_window->nextApdlPath = apdl;
        exportApdl->click();
        QVERIFY2(QFileInfo::exists(apdl), qPrintable(m_window->errors.join('\n')));
        const QStringList apdlLines = readLines(apdl);
        const QString expectedN = QString("N,%1,%2,%3,%4")
                                      .arg(moved)
                                      .arg(QString::fromStdString(NodeAsciiExporter::formatCoordinate(m.X())),
                                           QString::fromStdString(NodeAsciiExporter::formatCoordinate(m.Y())),
                                           QString::fromStdString(NodeAsciiExporter::formatCoordinate(m.Z())));
        QVERIFY(apdlLines.contains(expectedN));
        QVERIFY(apdlLines.contains("ET,1,165"));
        QVERIFY(apdlLines.contains("/PREP7"));
        QCOMPARE(apdlLines.filter(QRegularExpression("^N,")).size(), int(caseB->totalNodeCount()));
        QVERIFY(apdlLines.filter(QRegularExpression("^(MP|R|TYPE|MAT|D|F|SOLVE|EDCURVE),", QRegularExpression::CaseInsensitiveOption))
                    .isEmpty());
        QVERIFY(apdlLines.join('\n').contains("Geometria ajustada (E5.7.2.3)"));
        if (qEnvironmentVariableIsSet("CADMECANICO_EVIDENCE_DIR"))
        {
            const QString dir = qEnvironmentVariable("CADMECANICO_EVIDENCE_DIR");
            QFile::remove(dir + "/placa_furo_projecao_gui_PREP7.inp");
            QFile::copy(apdl, dir + "/placa_furo_projecao_gui_PREP7.inp");
        }

        // Voltar a uma opção antiga restaura a grade cartesiana.
        criterion->setCurrentIndex(1);
        generate->click();
        QVERIFY(!m_window->discretization()->adjusted());
        QVERIFY(firstMovedVertex(*m_window->discretization()) == 0);
        closeButton->click();
        reached = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reached);
    QVERIFY(m_window->errors.isEmpty());

    // O sólido e o documento não foram alterados.
    QVERIFY(m_window->document().shape().IsSame(original));
    QCOMPARE(m_window->document().revision(), revision);
}

QTEST_MAIN(TestE5723Ui)
#include "test_e5723_ui.moc"
