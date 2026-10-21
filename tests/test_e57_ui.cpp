// E5.7 na interface real, com contexto OpenGL real:
//  P1 — fluxo rápido: face plana selecionada + ferramenta de desenho (ou Novo esboço) abre o
//       esboço sobre a face com vista normal; concluir abre o diálogo de extrusão (faces
//       superior, lateral e inclinada; cancelamento; opção desativada; perfil aberto; persistência);
//  P4 — diálogo único de extrusão (tipo de operação, Unir resultado, Cortar o lado de fora,
//       destino, condição final, nome automático, edição) com o diálogo real;
//  P2 — painel Diagnóstico (documento válido, alteração recusada, abertura de documento com
//       erro, operação bloqueada, correção de parâmetros e recuperação).
#include "ViewerTestUtils.h"

#include "app/ExtrusionDialog.h"
#include "app/MainWindow.h"
#include "document/DocumentFile.h"
#include "modeling/FaceNaming.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QTimer>
#include <QTreeWidget>

#include <BRepAdaptor_Curve.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <functional>
#include <optional>

using cad::app::ExtrusionDialog;
using cad::app::MainWindow;
using cad::document::EndCondition;
using cad::document::ExtrudeMode;
using cad::document::ExtrudeParameters;
using cad::document::OperationStatus;
using cad::document::OperationType;
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
    // Extrusão: 'nextExtrusion' é devolvido (com esboço e destino iniciais); 'useRealExtrusionDialog'
    // abre o diálogo real.
    std::optional<ExtrudeParameters> nextExtrusion;
    bool useRealExtrusionDialog = false;
    QList<ExtrudeParameters> extrusionAsked;
    int planeAsked = 0;
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    QString nextSavePath, nextOpenPath;
    QStringList errors, infos;

protected:
    std::optional<PlaneType> askSketchPlane() override
    {
        ++planeAsked;
        return nextPlane;
    }
    std::optional<ExtrudeParameters> askExtrusion(const ExtrudeParameters& initial, bool editing) override
    {
        extrusionAsked << initial;
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
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
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

} // namespace

class TestE57Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // P1
    void fluxoRapidoFaceSuperiorComRetangulo();
    void fluxoRapidoFaceLateralPeloNovoEsboco();
    void fluxoRapidoFaceInclinada();
    void fluxoRapidoCancelamentoEOpcao();
    void fluxoRapidoPersistencia();
    // P4
    void dialogoUnicoTiposEOpcoes();
    void dialogoUnicoCorteExternoEEdicao();
    // P2
    void diagnosticoDocumentoValidoERecusa();
    void diagnosticoAberturaComErroECorrecao();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        return toLogical(viewer(), project(viewer(), m_window->editingSketch()->plane().toGlobal(u, v)));
    }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    void block();
    void iso()
    {
        action("actionVistaIsometrica")->trigger();
        QVERIFY(waitFrames(viewer()));
    }
    bool selectFaceAt(double x, double y, double z)
    {
        viewer()->clearSelection();
        if (!click(viewer(), at3d(x, y, z)))
            return false;
        pauseBetweenClicks();
        return m_window->viewer()->selectedShapes().size() == 1;
    }
    void twoClicks(QPointF a, QPointF b)
    {
        QVERIFY(click(viewer(), a));
        pauseBetweenClicks();
        QVERIFY(click(viewer(), b));
        pauseBetweenClicks();
    }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE57Ui::init()
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

void TestE57Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE57Ui::block()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0}));
    iso();
}

// --- P1 ------------------------------------------------------------------------

void TestE57Ui::fluxoRapidoFaceSuperiorComRetangulo()
{
    block();
    // Sem face selecionada as ferramentas de desenho ficam indisponíveis fora do esboço.
    QVERIFY(!action("actionFerramentaRetangulo")->isEnabled());
    QVERIFY(selectFaceAt(50, 30, 20));
    QTRY_VERIFY(action("actionFerramentaRetangulo")->isEnabled());
    // 1–4. Face plana selecionada + Retângulo: esboço sobre a face, vista normal, ferramenta ativa.
    action("actionFerramentaRetangulo")->trigger();
    QVERIFY(m_window->isEditingSketch());
    QVERIFY(m_window->editingSketch()->plane().type() == PlaneType::Face);
    QVERIFY(viewer()->sketchTool() == cad::viewer::SketchTool::Rectangle);
    QVERIFY(action("actionFerramentaRetangulo")->isChecked());
    QVERIFY(viewer()->view()->Camera()->Direction().IsEqual(gp_Dir(0, 0, -1), 1e-6));
    QCOMPARE(m_window->planeAsked, 0);
    // 5. Desenho direto (a ferramenta já está ativa).
    twoClicks(at(30, 20), at(70, 40));
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E57_01_fluxo_rapido_retangulo_na_face");
    // 6. Concluir pela interface: diálogo de extrusão aberto (saliência sobre o corpo de suporte).
    m_window->nextExtrusion = ExtrudeParameters{"", "", 10.0, false, ExtrudeMode::Boss};
    action("actionConcluirEsboco")->trigger();
    QCOMPARE(m_window->extrusionAsked.size(), 1);
    QVERIFY(m_window->extrusionAsked.first().mode == ExtrudeMode::Boss);
    QCOMPARE(m_window->extrusionAsked.first().targetId, std::string("op-1"));
    QCOMPARE(m_window->extrusionAsked.first().sketchId, std::string("sk-2"));
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    const auto& rect = std::get<SketchRectangle>(m_window->document().findSketch("sk-2")->entities()[0].geometry);
    QVERIFY(std::fabs(rect.width - 40.0) < 0.35 && std::fabs(rect.height - 20.0) < 0.35);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox + rect.width * rect.height * 10.0)) < 1e-6);
}

void TestE57Ui::fluxoRapidoFaceLateralPeloNovoEsboco()
{
    block();
    // Face lateral direita (x = 100) e "Novo esboço": sem perguntar o plano.
    QVERIFY(selectFaceAt(100, 30, 10));
    action("actionNovoEsboco")->trigger();
    QVERIFY(m_window->isEditingSketch());
    QCOMPARE(m_window->planeAsked, 0);
    const Sketch* s = m_window->editingSketch();
    QVERIFY(s->support() && s->plane().normal().IsEqual(gp_Dir(1, 0, 0), 1e-9));
    QVERIFY(viewer()->view()->Camera()->Direction().IsEqual(gp_Dir(-1, 0, 0), 1e-6));
    action("actionFerramentaCirculo")->trigger();
    twoClicks(at(30, 10), at(35, 10));
    QCOMPARE(s->entities().size(), size_t(1));
    // Cancelar o diálogo de extrusão: esboço gravado, nenhuma operação criada.
    m_window->nextExtrusion.reset();
    action("actionConcluirEsboco")->trigger();
    QCOMPARE(m_window->extrusionAsked.size(), 1);
    QCOMPARE(m_window->document().sketches().size(), size_t(2));
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    // Extrusão depois, pelo comando único: perfil fechado, corte passante.
    m_window->selectTreeItem("sk-2");
    ExtrudeParameters cut{"", "", 10.0, false, ExtrudeMode::Cut};
    cut.endCondition = EndCondition::ThroughAll;
    m_window->nextExtrusion = cut;
    action("actionExtrusaoUnificada")->trigger();
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    const double r = std::get<SketchCircle>(m_window->document().findSketch("sk-2")->entities()[0].geometry).radius;
    QVERIFY(std::fabs(r - 5.0) < 0.35);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox - kPi * r * r * 100.0)) < 1e-4);
}

void TestE57Ui::fluxoRapidoFaceInclinada()
{
    block();
    // Chamfer 3D de 10 mm na aresta superior frontal: face plana inclinada (normal (0,−1,1)/√2).
    const TopoDS_Edge edge = edgeAt(m_window->document().findOperation("op-1")->result, 50, 0, 20);
    QVERIFY(!edge.IsNull());
    QVERIFY(m_window->addFinish(OperationType::Chamfer3D, "op-1", {edge}, 10.0));
    iso();
    QVERIFY(selectFaceAt(50, 5, 15));
    action("actionFerramentaCirculo")->trigger();
    QVERIFY(m_window->isEditingSketch());
    const gp_Dir n = m_window->editingSketch()->plane().normal();
    QVERIFY2(n.IsEqual(gp_Dir(0, -1, 1), 1e-6), qPrintable(QStringLiteral("normal (%1; %2; %3)").arg(n.X()).arg(n.Y()).arg(n.Z())));
    QVERIFY(viewer()->view()->Camera()->Direction().IsOpposite(n, 1e-6));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E57_02_fluxo_rapido_face_inclinada");
    action("actionCancelarEsboco")->trigger();
    QVERIFY(m_window->extrusionAsked.isEmpty());
}

void TestE57Ui::fluxoRapidoCancelamentoEOpcao()
{
    block();
    // Cancelar o esboço: nenhum diálogo, documento intacto.
    QVERIFY(selectFaceAt(50, 30, 20));
    action("actionFerramentaLinha")->trigger();
    twoClicks(at(10, 10), at(50, 10));
    action("actionCancelarEsboco")->trigger();
    QVERIFY(m_window->extrusionAsked.isEmpty());
    QCOMPARE(m_window->document().sketches().size(), size_t(1));
    // Perfil aberto: esboço concluído sem abrir a extrusão.
    QVERIFY(selectFaceAt(50, 30, 20));
    action("actionFerramentaLinha")->trigger();
    twoClicks(at(10, 10), at(50, 10));
    action("actionConcluirEsboco")->trigger();
    QVERIFY(m_window->extrusionAsked.isEmpty());
    QCOMPARE(m_window->document().sketches().size(), size_t(2));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("sem perfil fechado")));
    // Opção desativada: perfil fechado, sem diálogo.
    action("actionExtrusaoAoConcluir")->setChecked(false);
    QVERIFY(selectFaceAt(50, 30, 20));
    action("actionFerramentaCirculo")->trigger();
    twoClicks(at(50, 30), at(55, 30));
    action("actionConcluirEsboco")->trigger();
    QVERIFY(m_window->extrusionAsked.isEmpty());
    QCOMPARE(m_window->document().sketches().size(), size_t(3));
    // Esboço em plano global (fluxo normal) não abre a extrusão ao concluir.
    action("actionExtrusaoAoConcluir")->setChecked(true);
    viewer()->clearSelection();
    action("actionNovoEsboco")->trigger();
    QCOMPARE(m_window->planeAsked, 1);
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 10, 10}));
    action("actionConcluirEsboco")->trigger();
    QVERIFY(m_window->extrusionAsked.isEmpty());
}

void TestE57Ui::fluxoRapidoPersistencia()
{
    block();
    QVERIFY(selectFaceAt(50, 30, 20));
    action("actionFerramentaCirculo")->trigger();
    twoClicks(at(50, 30), at(60, 30));
    m_window->nextExtrusion = ExtrudeParameters{"", "", 5.0, false, ExtrudeMode::Boss};
    action("actionConcluirEsboco")->trigger();
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    const double volume = ShapeInspector::volume(m_window->part());
    const QString path = m_dir->filePath("p1.cadmec");
    QVERIFY(m_window->saveDocumentTo(path));
    QVERIFY(m_window->openDocumentFrom(path));
    QVERIFY(m_window->infos.isEmpty()); // aberto sem erros
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(m_window->document().findSketch("sk-2")->support().has_value());
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - volume) < 1e-6);
}

// --- P4 ------------------------------------------------------------------------

void TestE57Ui::dialogoUnicoTiposEOpcoes()
{
    block();
    QVERIFY(selectFaceAt(50, 30, 20));
    action("actionFerramentaCirculo")->trigger();
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50, 30, 10}));
    action("actionExtrusaoAoConcluir")->setChecked(false);
    action("actionConcluirEsboco")->trigger();
    m_window->selectTreeItem("sk-2");
    m_window->useRealExtrusionDialog = true;
    bool checked = false;
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); }); // falha não deixa o diálogo aberto
        auto* type = d->findChild<QComboBox*>("campoTipoOperacao");
        auto* merge = d->findChild<QCheckBox*>("opcaoUnirResultado");
        auto* outside = d->findChild<QCheckBox*>("opcaoCortarLadoDeFora");
        auto* target = d->findChild<QComboBox*>("campoCorpoDestino");
        auto* end = d->findChild<QComboBox*>("campoCondicaoFinal");
        auto* name = d->findChild<QLineEdit*>("campoNomeExtrusao");
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY(type && merge && outside && target && end && name && message);
        // Padrão para esboço sobre face: adicionar material, unido ao corpo de suporte.
        QCOMPARE(type->count(), 3);
        QVERIFY(d->mode() == ExtrudeMode::Boss);
        QCOMPARE(d->windowTitle(), QStringLiteral("Extrusão de Saliência"));
        QVERIFY(merge->isChecked() && merge->isEnabled() && !outside->isEnabled() && target->isEnabled());
        QCOMPARE(name->text(), QStringLiteral("Extrusão de Saliência 1"));
        QVERIFY2(message->text().contains(QStringLiteral("123.141,59")), qPrintable(message->text()));
        // Desmarcar "Unir resultado" = novo corpo (nome acompanha o tipo enquanto não editado).
        merge->setChecked(false);
        QVERIFY(d->mode() == ExtrudeMode::NewBody);
        QCOMPARE(d->windowTitle(), QStringLiteral("Extrusão"));
        QVERIFY(!target->isEnabled());
        QCOMPARE(name->text(), QStringLiteral("Extrusão 2"));
        QVERIFY2(message->text().contains(QStringLiteral("3.141,59")), qPrintable(message->text()));
        // Remover material: Cortar o lado de fora habilitado; Unir resultado fixo; atravessar tudo.
        type->setCurrentIndex(type->findData(int(ExtrudeMode::Cut)));
        QCOMPARE(d->windowTitle(), QStringLiteral("Extrusão de Corte"));
        QVERIFY(outside->isEnabled() && !merge->isEnabled() && merge->isChecked());
        QCOMPARE(name->text(), QStringLiteral("Extrusão de Corte 1"));
        end->setCurrentIndex(end->findData(int(EndCondition::ThroughAll)));
        QVERIFY2(message->text().contains(QStringLiteral("113.716,81")), qPrintable(message->text()));
        // Voltar a adicionar: atravessar tudo deixa de valer (só no corte).
        type->setCurrentIndex(type->findData(int(ExtrudeMode::Boss)));
        QCOMPARE(end->currentData().toInt(), int(EndCondition::Blind));
        auto* model = qobject_cast<QStandardItemModel*>(end->model());
        QVERIFY(model && !model->item(end->findData(int(EndCondition::ThroughAll)))->isEnabled());
        // Nome editado pelo usuário não é substituído ao trocar o tipo.
        QTest::keyClicks(name, QStringLiteral(" X"));
        type->setCurrentIndex(type->findData(int(ExtrudeMode::Cut)));
        QCOMPARE(name->text(), QStringLiteral("Extrusão de Saliência 1 X"));
        saveEvidence(d->grab().toImage(), "E57_03_dialogo_unico");
        checked = true;
        d->reject(); // cancelamento: nada criado
    });
    action("actionExtrusaoUnificada")->trigger();
    QVERIFY(checked);
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
}

void TestE57Ui::dialogoUnicoCorteExternoEEdicao()
{
    block();
    QVERIFY(selectFaceAt(50, 30, 20));
    action("actionFerramentaCirculo")->trigger();
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50, 30, 10}));
    action("actionExtrusaoAoConcluir")->setChecked(false);
    action("actionConcluirEsboco")->trigger();
    m_window->selectTreeItem("sk-2");
    m_window->useRealExtrusionDialog = true;
    bool checked = false;
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); }); // falha não deixa o diálogo aberto
        d->findChild<QComboBox*>("campoTipoOperacao")->setCurrentIndex(2); // remover material
        d->findChild<QCheckBox*>("opcaoCortarLadoDeFora")->setChecked(true);
        auto* depth = d->findChild<QDoubleSpinBox*>("campoProfundidade");
        depth->setValue(5.0);
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY2(message->text().contains(QStringLiteral("91.570,80")), qPrintable(message->text()));
        // Pré-visualização: região removida (fora do perfil) em vermelho, destino em azul.
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->operandAPresentation()));
        iso();
        saveEvidence(m_window->grab().toImage(), "E57_04_previa_corte_externo");
        checked = true;
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    });
    action("actionExtrusaoUnificada")->trigger();
    QVERIFY(checked);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const auto* op = m_window->document().findOperation("op-2");
    QVERIFY(op && op->extrudeMode == ExtrudeMode::Cut && op->cutOutside);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (100.0 * 60.0 * 15.0 + kPi * 500.0)) < 1e-3);
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "operacao", "op-2"))
                .contains(QStringLiteral("Lado do corte: fora do perfil (interior preservado)")));
    m_window->selectTreeItem("op-2");
    QVERIFY(m_window->findChild<QLabel*>("painelPropriedades")->text().contains(QStringLiteral("Cortar o lado de fora: sim")));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E57_05_corte_externo");
    // Edição: o diálogo reabre com a opção marcada; alteração de profundidade e sentido.
    bool edited = false;
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); }); // falha não deixa o diálogo aberto
        QVERIFY(d->findChild<QCheckBox*>("opcaoCortarLadoDeFora")->isChecked());
        QCOMPARE(d->windowTitle(), QStringLiteral("Editar extrusão de corte"));
        d->findChild<QDoubleSpinBox*>("campoProfundidade")->setValue(8.0);
        edited = true;
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    });
    action("actionEditarOperacao")->trigger();
    QVERIFY(edited);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (100.0 * 60.0 * 12.0 + kPi * 800.0)) < 1e-3);
}

// --- P2 ------------------------------------------------------------------------

void TestE57Ui::diagnosticoDocumentoValidoERecusa()
{
    block();
    auto* dock = m_window->findChild<QDockWidget*>("dockDiagnostico");
    auto* summary = m_window->findChild<QLabel*>("rotuloResumoDiagnostico");
    QVERIFY(dock && summary && m_window->diagnosticsList());
    QVERIFY(dock->toggleViewAction()->isChecked()); // em abas com Propriedades
    QCOMPARE(dock->windowTitle(), QStringLiteral("Diagnóstico"));
    QCOMPARE(m_window->diagnosticsList()->topLevelItemCount(), 0);
    QVERIFY(summary->text().contains(QStringLiteral("Sem erros")));
    QCOMPARE(m_window->diagnosticsList()->columnCount(), 6);
    // Esboço aberto não usado: aviso.
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchLine{0, 0, 10, 0}));
    QVERIFY(m_window->finishSketch());
    QCOMPARE(m_window->diagnosticsList()->topLevelItemCount(), 1);
    QCOMPARE(m_window->diagnosticsList()->topLevelItem(0)->text(0), QStringLiteral("Aviso"));
    // Alteração recusada (corte que não intersecta): erro "não aplicada", documento intacto.
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50, 30, 5}));
    QVERIFY(m_window->finishSketch());
    // Sentido padrão do corte (−Z a partir de z = 0): fora do bloco.
    ExtrudeParameters cut{"", "sk-3", 5.0, false, ExtrudeMode::Cut, "op-1"};
    QVERIFY(!m_window->addExtrusion(cut));
    const auto list = m_window->diagnostics();
    const auto rejected = std::find_if(list.begin(), list.end(), [](const auto& d) {
        return d.state.find("Não aplicada") != std::string::npos;
    });
    QVERIFY(rejected != list.end());
    QVERIFY(rejected->guidance.find("sentido") != std::string::npos);
    QVERIFY(summary->text().contains(QStringLiteral("1 erro(s)")));
    QTRY_VERIFY(dock->isVisible()); // erro novo: a aba vem para a frente
    QCOMPARE(dock->windowTitle(), QStringLiteral("Diagnóstico (2)"));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E57_06_diagnostico_recusa");
    // Alteração válida seguinte (sentido invertido, +Z): a recusa deixa de ser atual.
    cut.reversed = true;
    QVERIFY(m_window->addExtrusion(cut));
    QCOMPARE(m_window->diagnosticsList()->topLevelItemCount(), 1); // só o aviso do esboço aberto
}

void TestE57Ui::diagnosticoAberturaComErroECorrecao()
{
    block();
    const int viewerWidth = viewer()->width();
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{50, 30, 10}));
    QVERIFY(m_window->finishSketch());
    ExtrudeParameters cut{"", "sk-2", 10.0, false, ExtrudeMode::Cut, "op-1", EndCondition::ThroughAll};
    cut.reversed = true; // a partir de z = 0 o corte passante vai para +Z
    QVERIFY(m_window->addExtrusion(cut));
    // Corpo independente e um Fillet 3D no resultado do corte (depende dele).
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{200, 0, 10, 10}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Independente", "sk-3", 5.0}));
    const TopoDS_Edge edge = edgeAt(m_window->document().findOperation("op-2")->result, 50, 0, 20);
    QVERIFY(m_window->addFinish(OperationType::Fillet3D, "op-2", {edge}, 2.0));
    const QString good = m_dir->filePath("valido.cadmec");
    QVERIFY(m_window->saveDocumentTo(good));
    // Arquivo alterado externamente: o corte volta ao sentido padrão (−Z a partir de z = 0) e não
    // intersecta o bloco.
    QFile in(good);
    QVERIFY(in.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    in.close();
    QJsonObject doc = root.value("document").toObject();
    QJsonArray ops = doc.value("operations").toArray();
    for (int i = 0; i < ops.size(); ++i)
    {
        QJsonObject op = ops[i].toObject();
        if (op.value("id").toString() == QLatin1String("op-2"))
            op.insert("reversed", false);
        ops[i] = op;
    }
    doc.insert("operations", ops);
    root.insert("document", doc);
    const QString broken = m_dir->filePath("com_erro.cadmec");
    QFile out(broken);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(QJsonDocument(root).toJson());
    out.close();

    m_window->newDocument();
    QVERIFY(m_window->openDocumentFrom(broken));
    QVERIFY(m_window->errors.isEmpty());
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(m_window->infos.first().contains(QStringLiteral("2 operação(ões) inválida(s)")));
    const auto& d = m_window->document();
    QCOMPARE(d.operations().size(), size_t(4)); // nada descartado
    QVERIFY(d.findOperation("op-2")->status == OperationStatus::Invalid);
    QVERIFY(d.findOperation("op-4")->status == OperationStatus::Invalid && d.findOperation("op-4")->blockedBy == "op-2");
    QVERIFY(d.findOperation("op-3")->status == OperationStatus::Valid);
    // Geometria exibida: bloco (último estado válido) + corpo independente.
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox + 500.0)) < 1e-6);
    // Painel: dois erros com operação, descrição, causa, orientação e estado.
    QTreeWidget* list = m_window->diagnosticsList();
    QCOMPARE(list->topLevelItemCount(), 2);
    QTreeWidgetItem* first = list->topLevelItem(0);
    QCOMPARE(first->text(0), QStringLiteral("Erro"));
    QVERIFY(first->text(1).contains(QStringLiteral("op-2")));
    QVERIFY2(first->text(2).contains(QStringLiteral("não está no sentido do corte")), qPrintable(first->text(2)));
    QVERIFY(first->text(3).contains(QStringLiteral("não alcança")));
    QVERIFY(first->text(4).contains(QStringLiteral("sentido")));
    QCOMPARE(first->text(5), QStringLiteral("Inválida"));
    QCOMPARE(list->topLevelItem(1)->text(5), QStringLiteral("Inválida (bloqueada)"));
    // Detalhe por extenso da linha selecionada; o painel em primeiro plano não estreita o
    // visualizador.
    const QString detail = m_window->findChild<QLabel*>("rotuloDetalheDiagnostico")->text();
    QVERIFY2(detail.contains(QStringLiteral("Causa provável")) && detail.contains(QStringLiteral("Como corrigir"))
                 && detail.contains(QStringLiteral("op-2")),
             qPrintable(detail));
    QVERIFY(m_window->findChild<QDockWidget*>("dockDiagnostico")->isVisible());
    QTRY_COMPARE(viewer()->width(), viewerWidth);
    // Árvore: estado com o motivo.
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "operacao", "op-2")).last().startsWith(QStringLiteral("Estado: Inválida — ")));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E57_07_documento_com_erro");
    // Duplo clique: seleciona a operação na árvore.
    emit list->itemDoubleClicked(first, 0);
    QCOMPARE(m_window->selectedOperationId(), QStringLiteral("op-2"));
    // Persistência: salvar sem corrigir mantém os parâmetros como estão.
    const QString resaved = m_dir->filePath("resalvo.cadmec");
    QVERIFY(m_window->saveDocumentTo(resaved));
    // Correção de parâmetros pela edição: cadeia recalculada, documento válido.
    ExtrudeParameters fixedParams = d.extrudeParameters("op-2");
    fixedParams.reversed = true;
    QVERIFY(m_window->updateExtrusion("op-2", fixedParams));
    QVERIFY(!m_window->document().hasInvalidOperations());
    QCOMPARE(list->topLevelItemCount(), 0);
    QVERIFY(m_window->findChild<QLabel*>("rotuloResumoDiagnostico")->text().contains(QStringLiteral("Sem erros")));
    QVERIFY(m_window->document().findOperation("op-4")->status == OperationStatus::Valid);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E57_08_documento_corrigido");
    // O arquivo regravado antes da correção reabre com o mesmo erro.
    m_window->infos.clear();
    QVERIFY(m_window->openDocumentFrom(resaved));
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(m_window->document().findOperation("op-2")->status == OperationStatus::Invalid);
}

QTEST_MAIN(TestE57Ui)
#include "test_e57_ui.moc"
