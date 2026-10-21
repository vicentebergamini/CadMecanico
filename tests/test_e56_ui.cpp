// E5.6 na interface real, com contexto OpenGL real: seleção de face plana pelo clique, rejeição
// de face curva, Criar Esboço na Face com vista normal (faces superior, frontal, lateral e
// inclinada), cancelamento sem alterações, Extrusão de Corte (atravessar tudo, pelo diálogo
// real com pré-visualização), Extrusão de Saliência, saliência sem contato com opção de novo
// corpo, Extrusão de Novo Corpo, Chamfer 2D avançado pelo diálogo, Chamfer 3D com duas
// distâncias / distância–ângulo e face de referência, e o fluxo integrado da seção 25.
#include "StlTestUtils.h"
#include "ViewerTestUtils.h"

#include "app/ExtrusionDialog.h"
#include "app/FinishDialog.h"
#include "app/MainWindow.h"
#include "app/SketchEntityDialog.h"
#include "document/DocumentFile.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <AIS_ViewCube.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>

using cad::app::FinishDialog;
using cad::app::MainWindow;
using cad::document::Chamfer3DMode;
using cad::document::EndCondition;
using cad::document::ExtrudeMode;
using cad::document::ExtrudeParameters;
using cad::document::OperationType;
using cad::modeling::BooleanKind;
using cad::modeling::ShapeInspector;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBox = 120000.0;

class TestableMainWindow : public MainWindow
{
public:
    std::optional<ExtrudeParameters> nextExtrusion;
    bool useRealExtrusionDialog = false;
    ExtrudeParameters lastInitial;
    std::optional<SketchChamfer> nextChamfer;
    bool useRealChamferDialog = false;
    bool convertToNewBody = false;
    QStringList convertAsked;
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    std::optional<StlExportRequest> nextStl;
    QString nextSavePath, nextOpenPath, nextExportPath;
    QStringList errors, infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    std::optional<ExtrudeParameters> askExtrusion(const ExtrudeParameters& initial, bool editing) override
    {
        lastInitial = initial;
        if (useRealExtrusionDialog)
            return MainWindow::askExtrusion(initial, editing);
        if (!nextExtrusion)
            return std::nullopt;
        ExtrudeParameters p = *nextExtrusion;
        p.sketchId = initial.sketchId;
        if (p.mode != ExtrudeMode::NewBody && p.targetId.empty())
            p.targetId = initial.targetId;
        return p;
    }
    std::optional<SketchChamfer> askChamfer2D(const SketchChamfer& initial) override
    {
        if (useRealChamferDialog)
            return MainWindow::askChamfer2D(initial);
        if (!nextChamfer)
            return std::nullopt;
        SketchChamfer c = *nextChamfer;
        c.entityA = initial.entityA;
        c.entityB = initial.entityB;
        c.corner = initial.corner;
        return c;
    }
    bool askConvertToNewBody(const QString& message) override
    {
        convertAsked << message;
        return convertToNewBody;
    }
    std::optional<StlExportRequest> askStlExport(const QString&) override { return nextStl; }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    QString askExportPath(const QString&) override { return nextExportPath; }
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

QTreeWidgetItem* topItem(QTreeWidget* tree, const QString& kind, const QString& id)
{
    QTreeWidgetItem* root = tree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
        if (root->child(i)->data(0, MainWindow::kItemIdRole).toString() == id
            && root->child(i)->data(0, MainWindow::kItemKindRole).toString() == kind)
            return root->child(i);
    return nullptr;
}

QStringList childTexts(const QTreeWidgetItem* item)
{
    QStringList t;
    for (int i = 0; item && i < item->childCount(); ++i)
        t << item->child(i)->text(0);
    return t;
}

bool inside(const TopoDS_Shape& s, double x, double y, double z)
{
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-7);
    return c.State() == TopAbs_IN;
}

} // namespace

class TestE56Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void selecaoDeFacePlana();
    void rejeicaoDeFaceCurva();
    void esbocoNaFaceSuperior();
    void vistaNormalNasFaces_data();
    void vistaNormalNasFaces();
    void cancelamentoSemAlteracoes();
    void cortePeloDialogoAtravessarTudo();
    void salienciaNaFaceLateral();
    void salienciaSemContatoOfereceNovoCorpo();
    void novoCorpoIndependenteEBooleana();
    void chamfer2DAvancadoPeloDialogo();
    void chamfer3DAvancadoPeloDialogo();
    void fluxoSolidWorks();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        return toLogical(viewer(), project(viewer(), m_window->editingSketch()->plane().toGlobal(u, v)));
    }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    // Bloco 100 x 60 x 20 (Extrusão de Novo Corpo do esboço XY) na vista isométrica.
    void block();
    // Clique na face e "Criar Esboço na Face".
    bool sketchOnFaceAt(double x, double y, double z);
    void iso()
    {
        action("actionVistaIsometrica")->trigger();
        QVERIFY(waitFrames(viewer()));
    }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE56Ui::init()
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

void TestE56Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE56Ui::block()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0}));
    iso();
}

bool TestE56Ui::sketchOnFaceAt(double x, double y, double z)
{
    m_window->viewer()->clearSelection();
    if (!click(viewer(), at3d(x, y, z)))
        return false;
    QTest::qWait(200);
    pauseBetweenClicks();
    if (m_window->viewer()->selectedShapes().size() != 1)
        return false;
    action("actionEsbocoNaFace")->trigger();
    return m_window->isEditingSketch();
}

void TestE56Ui::selecaoDeFacePlana()
{
    block();
    QVERIFY(click(viewer(), at3d(50, 30, 20)));
    QTRY_COMPARE(viewer()->selectedShapes().size(), 1);
    QCOMPARE(viewer()->selectedShapes().first().ShapeType(), TopAbs_FACE);
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("Plana")));
    QVERIFY(action("actionEsbocoNaFace")->isEnabled());
    QVERIFY(action("actionEsbocoNaFace")->toolTip().contains(QStringLiteral("face plana selecionada")));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E56_01_face_superior_selecionada");
    // Sem face selecionada: o comando explica.
    viewer()->clearSelection();
    action("actionEsbocoNaFace")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("face plana")));
    QVERIFY(!m_window->isEditingSketch());
}

void TestE56Ui::rejeicaoDeFaceCurva()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{0, 0, 20}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Cilindro", "sk-1", 30.0}));
    iso();
    // Ponto da superfície cilíndrica voltado para a câmera isométrica (+X, −Y).
    QVERIFY(click(viewer(), at3d(20.0 * std::cos(-kPi / 4), 20.0 * std::sin(-kPi / 4), 15.0)));
    QTRY_COMPARE(viewer()->selectedShapes().size(), 1);
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("Cilíndrica")));
    action("actionEsbocoNaFace")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("não é plana")));
    QVERIFY(!m_window->isEditingSketch());
    QVERIFY(m_window->document().sketches().size() == 1);
}

void TestE56Ui::esbocoNaFaceSuperior()
{
    block();
    QVERIFY(sketchOnFaceAt(50, 30, 20));
    // 3/4. Esboço associado à face, vista normal à face (olhando −Z, V = +Y), sólido oculto.
    const Sketch* s = m_window->editingSketch();
    QVERIFY(s->support() && s->support()->bodyId == "op-1");
    QVERIFY(s->plane().type() == PlaneType::Face);
    QVERIFY(s->plane().origin().Distance(gp_Pnt(0, 0, 20)) < 1e-9); // 5. coordenadas locais na face
    const auto& cam = viewer()->view()->Camera();
    QVERIFY(cam->Direction().IsEqual(gp_Dir(0, 0, -1), 1e-6));
    QVERIFY(cam->OrthogonalizedUp().IsEqual(gp_Dir(0, 1, 0), 1e-6));
    QVERIFY(cam->IsOrthographic());
    QVERIFY(viewer()->isSketchMode());
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(m_window->operationsTree()->currentItem()->text(0).contains(QStringLiteral("em edição"))
            || topItem(m_window->operationsTree(), "esboco", "sk-2"));
    // Círculo pelo mouse no plano da face.
    action("actionFerramentaCirculo")->trigger();
    QVERIFY(click(viewer(), at(50.0, 30.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(60.0, 30.0)));
    pauseBetweenClicks();
    const auto& c = std::get<SketchCircle>(m_window->editingSketch()->entities()[0].geometry);
    qInfo("Circulo na face: centro (%.2f; %.2f) R %.2f", c.cx, c.cy, c.radius);
    QVERIFY(std::fabs(c.cx - 50.0) < 0.35 && std::fabs(c.cy - 30.0) < 0.35 && std::fabs(c.radius - 10.0) < 0.35);
    // O círculo é exibido sobre a face (z = 20) em coordenadas globais.
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E56_02_esboco_na_face_vista_normal");
    QVERIFY(m_window->finishSketch());
    // Ao concluir: navegação 3D e sólido restaurados; árvore com o suporte.
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    QTreeWidgetItem* item = topItem(m_window->operationsTree(), "esboco", "sk-2");
    QVERIFY(item);
    QCOMPARE(item->text(0), QStringLiteral("Esboço 2 (sobre face de Bloco (op-1))"));
    QVERIFY(childTexts(item).first().startsWith(QStringLiteral("Suporte: face plana de Bloco (op-1), normal (0,00; 0,00; 1,00)")));
    auto* panel = m_window->findChild<QLabel*>("painelPropriedades");
    QVERIFY(panel->text().contains(QStringLiteral("face plana de Bloco")));
}

void TestE56Ui::vistaNormalNasFaces_data()
{
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");
    QTest::addColumn<double>("z");
    QTest::addColumn<double>("nx");
    QTest::addColumn<double>("ny");
    QTest::addColumn<double>("nz");
    QTest::newRow("frontal (XZ)") << 50.0 << 0.0 << 10.0 << 0.0 << -1.0 << 0.0;
    QTest::newRow("lateral (YZ)") << 100.0 << 30.0 << 10.0 << 1.0 << 0.0 << 0.0;
    QTest::newRow("inclinada") << 20.0 << -25.0 << 15.0 << 0.6 << 0.0 << 0.8;
}

void TestE56Ui::vistaNormalNasFaces()
{
    QFETCH(double, x);
    QFETCH(double, y);
    QFETCH(double, z);
    QFETCH(double, nx);
    QFETCH(double, ny);
    QFETCH(double, nz);
    if (nz > 0.5 && nx > 0.1)
    {
        // Prisma triangular com face inclinada (normal (0,6; 0; 0,8)).
        QVERIFY(m_window->startNewSketch(PlaneType::XZ));
        QVERIFY(m_window->addSketchGeometry(SketchLine{0, 0, 40, 0}));
        QVERIFY(m_window->addSketchGeometry(SketchLine{40, 0, 0, 30}));
        QVERIFY(m_window->addSketchGeometry(SketchLine{0, 30, 0, 0}));
        QVERIFY(m_window->finishSketch());
        QVERIFY(m_window->addExtrusion({"Prisma", "sk-1", 50.0}));
        iso();
    }
    else
        block();
    QVERIFY(sketchOnFaceAt(x, y, z));
    const SketchPlane& p = m_window->editingSketch()->plane();
    const gp_Dir n(nx, ny, nz);
    QVERIFY(p.normal().IsEqual(n, 1e-9));
    const auto& cam = viewer()->view()->Camera();
    qInfo("camera (%.3f; %.3f; %.3f) up (%.3f; %.3f; %.3f)", cam->Direction().X(), cam->Direction().Y(), cam->Direction().Z(),
          cam->OrthogonalizedUp().X(), cam->OrthogonalizedUp().Y(), cam->OrthogonalizedUp().Z());
    QVERIFY(cam->Direction().IsEqual(n.Reversed(), 1e-6));          // perpendicular à face
    QVERIFY(cam->OrthogonalizedUp().IsEqual(p.verticalAxis(), 1e-6)); // eixo vertical local para cima
    // Eixo horizontal local para a direita na tela, vertical para cima (sem espelhamento).
    const QPoint o = project(viewer(), p.toGlobal(0, 0));
    const QPoint h = project(viewer(), p.toGlobal(20, 0));
    const QPoint v = project(viewer(), p.toGlobal(0, 20));
    QVERIFY(h.x() > o.x() + 10 && std::abs(h.y() - o.y()) <= 1);
    QVERIFY(v.y() < o.y() - 10 && std::abs(v.x() - o.x()) <= 1);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), QStringLiteral("E56_03_vista_normal_%1").arg(QTest::currentDataTag()).replace(' ', '_'));
    action("actionCancelarEsboco")->trigger();
}

void TestE56Ui::cancelamentoSemAlteracoes()
{
    block();
    const int revision = m_window->document().revision();
    QVERIFY(sketchOnFaceAt(50, 30, 20));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50, 30, 5}));
    action("actionCancelarEsboco")->trigger();
    QVERIFY(!m_window->isEditingSketch());
    QCOMPARE(m_window->document().sketches().size(), size_t(1));
    QCOMPARE(m_window->document().revision(), revision);
    // Câmera e navegação consistentes: ViewCube, sólido, vistas padrão e rotação funcionam.
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(action("actionVistaFrente")->isEnabled());
    const gp_Dir dir = viewer()->view()->Camera()->Direction();
    const QPointF c(viewer()->width() / 2.0, viewer()->height() / 2.0);
    QVERIFY(drag(viewer(), c, c + QPointF(100, 40), Qt::MiddleButton));
    QVERIFY(dir.Angle(viewer()->view()->Camera()->Direction()) > 0.1);
    action("actionVistaFrente")->trigger();
    QVERIFY(viewer()->view()->Camera()->Direction().Angle(gp_Dir(0, 1, 0)) < 1e-3);
}

void TestE56Ui::cortePeloDialogoAtravessarTudo()
{
    block();
    QVERIFY(sketchOnFaceAt(50, 30, 20));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50, 30, 10}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(action("actionExtrusaoCorte")->isEnabled());
    m_window->useRealExtrusionDialog = true;
    bool checked = false;
    whenDialogOpens<cad::app::ExtrusionDialog>([&](cad::app::ExtrusionDialog* d) {
        auto* target = d->findChild<QComboBox*>("campoCorpoDestino");
        auto* end = d->findChild<QComboBox*>("campoCondicaoFinal");
        auto* depth = d->findChild<QDoubleSpinBox*>("campoProfundidade");
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY(target && end && depth && message);
        QCOMPARE(d->windowTitle(), QStringLiteral("Extrusão de Corte"));
        QCOMPARE(target->currentText(), QStringLiteral("Bloco (op-1)"));
        QVERIFY(d->findChild<QLabel*>("rotuloSentido")->text().contains(QStringLiteral("para dentro da face")));
        QCOMPARE(d->findChild<QLabel*>("rotuloEsbocoOrigem")->text(),
                 QStringLiteral("Esboço 2 (sk-2, sobre face de Bloco (op-1))"));
        // Profundidade definida: furo cego de 10 mm; ferramenta em vermelho, destino em azul.
        QVERIFY2(message->text().contains(QStringLiteral("116.858,41")), qPrintable(message->text()));
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->operandAPresentation()));
        end->setCurrentIndex(1); // Atravessar tudo
        QVERIFY(!depth->isEnabled());
        QVERIFY2(message->text().contains(QStringLiteral("113.716,81")), qPrintable(message->text()));
        QCOMPARE(m_window->document().operations().size(), size_t(1)); // pré-visualização não altera
        iso();
        saveEvidence(m_window->grab().toImage(), "E56_04_previa_corte_atravessar_tudo");
        saveEvidence(d->grab().toImage(), "E56_05_dialogo_corte");
        checked = true;
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    });
    m_window->selectTreeItem("sk-2");
    action("actionExtrusaoCorte")->trigger();
    QVERIFY(checked);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox - 2000.0 * kPi)) < 1e-4);
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
    const QStringList children = childTexts(topItem(m_window->operationsTree(), "operacao", "op-2"));
    qInfo("%s", qPrintable(children.join(" | ")));
    QVERIFY(children.contains(QStringLiteral("Tipo: Corte (remove do corpo)")));
    QVERIFY(children.contains(QStringLiteral("Corpo de destino: Bloco (op-1)")));
    QVERIFY(children.contains(QStringLiteral("Condição final: Atravessar tudo")));
    // Painel de propriedades coerente com a árvore (sentido do corte).
    m_window->selectTreeItem("op-2");
    const QString properties = m_window->findChild<QLabel*>("painelPropriedades")->text();
    QVERIFY2(properties.contains(QStringLiteral("Sentido: para dentro da face")), qPrintable(properties));
    // Furo passante visível na vista superior.
    action("actionVistaSuperior")->trigger();
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    const QImage background = backgroundReference(viewer());
    const QPoint hole = project(viewer(), gp_Pnt(50, 30, 20));
    QVERIFY2(!differs(pixelAt(img, hole), pixelAt(background, hole), 20), "furo não atravessa");
    saveEvidence(m_window->grab().toImage(), "E56_06_furo_passante");
}

void TestE56Ui::salienciaNaFaceLateral()
{
    block();
    QVERIFY(sketchOnFaceAt(50, 0, 10)); // face frontal (normal −Y)
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{70, 2, 20, 15}));
    QVERIFY(m_window->finishSketch());
    m_window->nextExtrusion = ExtrudeParameters{"", "", 10.0, false, ExtrudeMode::Boss};
    action("actionExtrusaoSaliencia")->trigger();
    QCOMPARE(m_window->lastInitial.targetId, std::string("op-1")); // destino = corpo de suporte
    QCOMPARE(m_window->lastInitial.name, std::string("Extrusão de Saliência 1"));
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox + 3000.0)) < 1e-6);
    QCOMPARE(m_window->document().solidCount(), 1);
    QVERIFY(inside(m_window->part(), 80, -5, 10)); // material adicionado fora da face frontal
    QVERIFY(topItem(m_window->operationsTree(), "operacao", "op-1")->text(0).contains(QStringLiteral("consumido por")));
    iso();
    saveEvidence(m_window->grab().toImage(), "E56_07_saliencia_face_frontal");
}

void TestE56Ui::salienciaSemContatoOfereceNovoCorpo()
{
    block();
    QVERIFY(sketchOnFaceAt(50, 30, 20));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{150, 10, 20, 20})); // fora da face
    QVERIFY(m_window->finishSketch());
    m_window->nextExtrusion = ExtrudeParameters{"", "", 10.0, false, ExtrudeMode::Boss};
    m_window->convertToNewBody = false;
    action("actionExtrusaoSaliencia")->trigger();
    QCOMPARE(m_window->convertAsked.size(), 1);
    QVERIFY(m_window->convertAsked.first().contains(QStringLiteral("não toca")));
    QCOMPARE(m_window->errors.size(), 1); // usuário recusou: nada criado
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    m_window->convertToNewBody = true;
    m_window->selectTreeItem("sk-2");
    action("actionExtrusaoSaliencia")->trigger();
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(m_window->document().operations()[1].extrudeMode == ExtrudeMode::NewBody);
    QCOMPARE(m_window->document().solidCount(), 2);
}

void TestE56Ui::novoCorpoIndependenteEBooleana()
{
    block();
    QVERIFY(sketchOnFaceAt(50, 30, 20));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{40, 20, 20, 20}));
    QVERIFY(m_window->finishSketch());
    m_window->nextExtrusion = ExtrudeParameters{"", "", 10.0, true}; // novo corpo para dentro do bloco
    action("actionExtrusao")->trigger();
    QCOMPARE(m_window->document().solidCount(), 2); // sem união automática
    QCOMPARE(m_window->document().activeBodies(), (std::vector<std::string>{"op-1", "op-2"}));
    QVERIFY(m_window->addBoolean({"", BooleanKind::Common, "op-1", "op-2"}));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - 4000.0) < 1e-6);
}

void TestE56Ui::chamfer2DAvancadoPeloDialogo()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QTest::keyClick(viewer(), Qt::Key_F);
    QVERIFY(waitFrames(viewer()));
    action("actionFerramentaChamfer2D")->trigger();
    m_window->useRealChamferDialog = true;
    bool checked = false;
    whenDialogOpens<cad::app::SketchEntityDialog>([&](cad::app::SketchEntityDialog* d) {
        auto* mode = d->findChild<QComboBox*>("dim_mode");
        auto* d1 = d->findChild<QDoubleSpinBox*>("dim_distance");
        auto* d2 = d->findChild<QDoubleSpinBox*>("dim_distance2");
        QVERIFY(mode && d1 && d2 && d->findChild<QDoubleSpinBox*>("dim_angle") && d->findChild<QCheckBox*>("dim_flip"));
        QCOMPARE(mode->count(), 3);
        QVERIFY(d->findChild<QLabel*>("rotuloConvencao")->text().contains(QStringLiteral("lado de referência")));
        mode->setCurrentIndex(1); // duas distâncias
        d1->setValue(5.0);
        d2->setValue(10.0);
        saveEvidence(d->grab().toImage(), "E56_08_dialogo_chamfer2d");
        checked = true;
        d->findChild<QDialogButtonBox*>("botoesDimensoes")->button(QDialogButtonBox::Ok)->click();
    });
    QVERIFY(click(viewer(), at(0.2, 0.2)));
    pauseBetweenClicks();
    QVERIFY(checked);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const auto& c = std::get<SketchChamfer>(m_window->editingSketch()->entities()[1].geometry);
    QVERIFY(c.mode == ChamferMode::TwoDistances && c.distance == 5.0 && c.distance2 == 10.0 && c.corner == 0);
    QVERIFY(m_window->finishSketch());
    QVERIFY(std::fabs(buildProfile(m_window->document().sketches()[0]).area - 5975.0) < 1e-9);
}

void TestE56Ui::chamfer3DAvancadoPeloDialogo()
{
    block();
    action("actionChamfer3D")->trigger();
    FinishDialog* d = m_window->finishDialog();
    QVERIFY(d && QTest::qWaitForWindowExposed(d));
    QVERIFY(click(viewer(), at3d(50, 0, 20)));
    QTRY_COMPARE(d->edgeCount(), 1);
    auto* mode = d->findChild<QComboBox*>("campoModoChanfro");
    auto* d1 = d->findChild<QDoubleSpinBox*>("campoValorAcabamento");
    auto* d2 = d->findChild<QDoubleSpinBox*>("campoDistancia2");
    auto* angle = d->findChild<QDoubleSpinBox*>("campoAngulo");
    auto* flip = d->findChild<QCheckBox*>("opcaoInverterReferencia");
    auto* reference = d->findChild<QLabel*>("rotuloFaceReferencia");
    QVERIFY(mode && d1 && d2 && angle && flip && reference);
    mode->setCurrentIndex(1);
    d1->setValue(3.0);
    d2->setValue(6.0);
    QVERIFY2(reference->text().contains(QStringLiteral("(0,00; 0,00; 1,00)")), qPrintable(reference->text()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->operandAPresentation())); // face de referência em azul
    QVERIFY2(d->resultMessage().contains(QStringLiteral("119.100,00")), qPrintable(d->resultMessage()));
    flip->setChecked(true); // a outra face (frontal)
    QVERIFY(reference->text().contains(QStringLiteral("(0,00; -1,00; 0,00)")));
    flip->setChecked(false);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E56_09_chamfer3d_duas_distancias");
    saveEvidence(d->grab().toImage(), "E56_10_dialogo_chamfer3d");
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox - 900.0)) < 1e-6);
    QVERIFY(viewer()->operandAPresentation().IsNull());
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "operacao", "op-2")).contains(QStringLiteral("Modo: duas distâncias (D2 = 6,00 mm)")));
    // Edição para distância–ângulo (D = 5, 30°).
    m_window->selectTreeItem("op-2");
    action("actionEditarOperacao")->trigger();
    d = m_window->finishDialog();
    QVERIFY(d);
    QTRY_COMPARE(d->edgeCount(), 1);
    QCOMPARE(d->findChild<QComboBox*>("campoModoChanfro")->currentIndex(), 1);
    QVERIFY(!d->findChild<QCheckBox*>("opcaoInverterReferencia")->isChecked());
    d->findChild<QComboBox*>("campoModoChanfro")->setCurrentIndex(2);
    d->findChild<QDoubleSpinBox*>("campoValorAcabamento")->setValue(5.0);
    d->findChild<QDoubleSpinBox*>("campoAngulo")->setValue(30.0);
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    const double expected = kBox - 0.5 * 5.0 * 5.0 * std::tan(kPi / 6.0) * 100.0;
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - expected) < 1e-4);
    QVERIFY(m_window->document().finishParameters("op-2").chamferMode == Chamfer3DMode::DistanceAngle);
}

void TestE56Ui::fluxoSolidWorks()
{
    // 1. Documento vazio.
    QVERIFY(m_window->document().operations().empty() && m_window->document().sketches().empty());
    // 2-3. Plano XY e retângulo 100 x 60 (mouse + dimensões).
    m_window->nextPlane = PlaneType::XY;
    action("actionNovoEsboco")->trigger();
    action("actionFerramentaRetangulo")->trigger();
    QVERIFY(click(viewer(), at(10, 10)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(40, 30)));
    pauseBetweenClicks();
    QVERIFY(m_window->updateSketchEntity("e-1", SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    // 4. Extrusão de Novo Corpo 20 mm.
    m_window->nextExtrusion = ExtrudeParameters{"", "", 20.0};
    action("actionExtrusao")->trigger();
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - kBox) < 1e-6);
    iso();
    // 5-7. Face superior, esboço sobre a face, vista normal.
    QVERIFY(sketchOnFaceAt(50, 30, 20));
    QVERIFY(viewer()->view()->Camera()->Direction().IsEqual(gp_Dir(0, 0, -1), 1e-6));
    // 8. Círculo R10 (mouse + dimensões).
    action("actionFerramentaCirculo")->trigger();
    QVERIFY(click(viewer(), at(50, 30)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(58, 30)));
    pauseBetweenClicks();
    QVERIFY(m_window->updateSketchEntity("e-1", SketchCircle{50, 30, 10}));
    QVERIFY(m_window->finishSketch());
    // 9. Extrusão de Corte → Atravessar tudo (diálogo real).
    m_window->useRealExtrusionDialog = true;
    whenDialogOpens<cad::app::ExtrusionDialog>([](cad::app::ExtrusionDialog* d) {
        d->findChild<QComboBox*>("campoCondicaoFinal")->setCurrentIndex(1);
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    });
    action("actionExtrusaoCorte")->trigger();
    m_window->useRealExtrusionDialog = false;
    // 10. Furo passante.
    bool through = true;
    for (double z = 0.25; z < 20.0; z += 0.5)
        through = through && !inside(m_window->part(), 50, 30, z);
    QVERIFY(through);
    const double afterCut = kBox - 2000.0 * kPi;
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - afterCut) < 1e-4);
    iso();
    saveEvidence(m_window->grab().toImage(), "E56_20_fluxo_furo_passante");
    // 11-13. Outra face plana (frontal), novo esboço, retângulo 20 x 15.
    QVERIFY(sketchOnFaceAt(30, 0, 10));
    QVERIFY(viewer()->view()->Camera()->Direction().IsEqual(gp_Dir(0, 1, 0), 1e-6));
    action("actionFerramentaRetangulo")->trigger();
    QVERIFY(click(viewer(), at(70, 2)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(85, 12)));
    pauseBetweenClicks();
    QVERIFY(m_window->updateSketchEntity("e-1", SketchRectangle{70, 2, 20, 15}));
    QVERIFY(m_window->finishSketch());
    // 14-15. Extrusão de Saliência 10 mm: material adicionado.
    m_window->nextExtrusion = ExtrudeParameters{"", "", 10.0, false, ExtrudeMode::Boss};
    action("actionExtrusaoSaliencia")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const double afterBoss = afterCut + 3000.0;
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - afterBoss) < 1e-4);
    QVERIFY(inside(m_window->part(), 80, -5, 10));
    iso();
    // 16-17. Aresta superior traseira → Chamfer 3D duas distâncias (3 na face superior, 6 na traseira).
    // (A aresta superior frontal não serve: D2 = 6 na face frontal alcançaria a saliência.)
    action("actionChamfer3D")->trigger();
    FinishDialog* d = m_window->finishDialog();
    QVERIFY(d && QTest::qWaitForWindowExposed(d));
    QVERIFY(click(viewer(), at3d(30, 60, 20)));
    QTRY_COMPARE(d->edgeCount(), 1);
    d->findChild<QComboBox*>("campoModoChanfro")->setCurrentIndex(1);
    d->findChild<QDoubleSpinBox*>("campoValorAcabamento")->setValue(3.0);
    d->findChild<QDoubleSpinBox*>("campoDistancia2")->setValue(6.0);
    qInfo("chamfer 3D: %s", qPrintable(d->resultMessage()));
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const QString chamferId = m_window->selectedOperationId();
    // 18. Edição para distância–ângulo (5 mm, 30°).
    action("actionEditarOperacao")->trigger();
    d = m_window->finishDialog();
    QVERIFY(d);
    QTRY_COMPARE(d->edgeCount(), 1);
    d->findChild<QComboBox*>("campoModoChanfro")->setCurrentIndex(2);
    d->findChild<QDoubleSpinBox*>("campoValorAcabamento")->setValue(5.0);
    d->findChild<QDoubleSpinBox*>("campoAngulo")->setValue(30.0);
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY(m_window->document().finishParameters(chamferId.toStdString()).chamferMode == Chamfer3DMode::DistanceAngle);
    // 19. Fillet 3D R2 na aresta superior direita.
    iso();
    action("actionFillet3D")->trigger();
    d = m_window->finishDialog();
    QVERIFY(d && QTest::qWaitForWindowExposed(d));
    QVERIFY(click(viewer(), at3d(100, 35, 20)));
    QTRY_COMPARE(d->edgeCount(), 1);
    d->findChild<QDoubleSpinBox*>("campoValorAcabamento")->setValue(2.0);
    d->findChild<QDialogButtonBox*>("botoesAcabamento")->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!m_window->finishDialog());
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const double finalVolume = ShapeInspector::volume(m_window->part());
    qInfo("volume final %.4f", finalVolume);
    QVERIFY(finalVolume < afterBoss && finalVolume > afterBoss - 1500.0);
    QVERIFY(ShapeInspector::isValid(m_window->part()));
    QCOMPARE(m_window->document().solidCount(), 1); // 27. sem duplicação de intermediários
    QCOMPARE(m_window->document().activeBodies().size(), size_t(1));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E56_21_fluxo_final");
    // 20-22. Salvar, fechar, reabrir e reconstruir.
    m_window->nextSavePath = m_dir->filePath("fluxo_e56.cadmec");
    action("actionSalvar")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QFile saved(m_window->nextSavePath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const QByteArray json = saved.readAll();
    saved.close();
    action("actionNovo")->trigger();
    QVERIFY(m_window->part().IsNull());
    m_window->nextOpenPath = m_window->nextSavePath;
    action("actionAbrir")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(5));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - finalVolume) < 1e-6);
    QVERIFY(cad::document::DocumentFile::toJson(m_window->document()) == json);
    QVERIFY(m_window->document().sketches()[1].support() && m_window->document().sketches()[2].support());
    // 23-26. STEP, STL binário e ASCII, verificação.
    m_window->nextExportPath = m_dir->filePath("fluxo_e56.step");
    action("actionExportarStep")->trigger();
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = ShapeInspector::inspect(read.shape);
    qInfo("STEP: %d solido(s), %d faces, volume %.4f, valido %d", s.solids, s.faces, s.volume, s.isValid);
    QVERIFY(s.isValid && s.solids == 1 && std::fabs(s.volume - finalVolume) < 1e-3);
    for (auto format : {cad::io::StlFormat::Binary, cad::io::StlFormat::Ascii})
    {
        const QString path = m_dir->filePath(format == cad::io::StlFormat::Binary ? "fluxo_b.stl" : "fluxo_a.stl");
        cad::io::StlExportOptions o;
        o.format = format;
        o.quality = cad::io::StlQuality::High;
        m_window->nextStl = MainWindow::StlExportRequest{path, o};
        action("actionExportarStl")->trigger();
        const auto mesh = cad::test::stl::read(path.toStdString());
        QVERIFY2(mesh.ok, mesh.error.c_str());
        qInfo("STL %s: %d triangulos, volume %.3f, caixa [%.2f %.2f %.2f]-[%.2f %.2f %.2f]", mesh.binary ? "binario" : "ASCII",
              int(mesh.triangles.size()), mesh.volume(), mesh.min.x, mesh.min.y, mesh.min.z, mesh.max.x, mesh.max.y, mesh.max.z);
        QVERIFY(mesh.finite() && mesh.degenerate() == 0);
        QVERIFY(std::fabs(mesh.volume() - finalVolume) / finalVolume < 0.002);
        QVERIFY(std::fabs(mesh.min.y + 10.0) < 1e-3 && std::fabs(mesh.max.z - 20.0) < 1e-3); // saliência e topo
    }
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    iso();
    saveEvidence(m_window->grab().toImage(), "E56_22_fluxo_reaberto");
}

QTEST_MAIN(TestE56Ui)
#include "test_e56_ui.moc"
