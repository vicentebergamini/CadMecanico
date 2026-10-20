// Extrusão paramétrica e exportação STL na interface real (E5.3), com contexto OpenGL real:
// documento inicial vazio, esboço criado pelo mouse, comando Extrusão (gancho e diálogo real com
// pré-visualização), árvore, visualizador, edição da profundidade e do esboço de origem,
// seleção no sólido extrudado, navegação, ViewCube, salvar/abrir, STEP, STL e fechamento.
#include "StlTestUtils.h"
#include "ViewerTestUtils.h"

#include "app/ExtrusionDialog.h"
#include "app/MainWindow.h"
#include "app/StlExportDialog.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <AIS_ViewCube.hxx>
#include <Graphic3d_Camera.hxx>
#include <gp_Dir.hxx>

#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>

using cad::app::MainWindow;
using cad::document::ExtrudeParameters;
using cad::modeling::ShapeInspector;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

class TestableMainWindow : public MainWindow
{
public:
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    // Gancho da extrusão: vazio = cancelar; useRealExtrusionDialog = diálogo real.
    std::optional<ExtrudeParameters> nextExtrusion;
    bool useRealExtrusionDialog = false;
    ExtrudeParameters lastInitial;
    std::optional<StlExportRequest> nextStl;
    bool useRealStlDialog = false;
    bool overwrite = true;
    QString nextSavePath;
    QString nextOpenPath;
    QString nextExportPath;
    QStringList errors;
    QStringList infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    std::optional<ExtrudeParameters> askExtrusion(const ExtrudeParameters& initial, bool editing) override
    {
        lastInitial = initial;
        if (useRealExtrusionDialog)
            return MainWindow::askExtrusion(initial, editing);
        return nextExtrusion;
    }
    std::optional<StlExportRequest> askStlExport(const QString& suggested) override
    {
        if (useRealStlDialog)
            return MainWindow::askStlExport(suggested);
        return nextStl;
    }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    QString askExportPath(const QString&) override { return nextExportPath; }
    bool confirmOverwrite(const QString&) override { return overwrite; }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
};

QTreeWidgetItem* topItem(QTreeWidget* tree, const QString& kind, const QString& id)
{
    QTreeWidgetItem* root = tree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
    {
        QTreeWidgetItem* item = root->child(i);
        if (item->data(0, MainWindow::kItemIdRole).toString() == id
            && item->data(0, MainWindow::kItemKindRole).toString() == kind)
            return item;
    }
    return nullptr;
}

QStringList childTexts(const QTreeWidgetItem* item)
{
    QStringList texts;
    for (int i = 0; item && i < item->childCount(); ++i)
        texts << item->child(i)->text(0);
    return texts;
}

// Executa `body` com o diálogo modal ativo do tipo T assim que ele aparecer.
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

constexpr double kBoxVolume = 100.0 * 60.0 * 20.0;

} // namespace

class TestExtrusaoUi : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void documentoInicialVazio();
    void comandoSemEsbocoExplica();
    void esbocoPeloMouseEExtrusao();
    void cancelarNaoAltera();
    void dialogoRealComPreVisualizacao();
    void arvoreDaExtrusao();
    void visualizadorExibeSolido();
    void editarProfundidade();
    void editarEsbocoRecalcula();
    void edicaoDeEsbocoInvalidaMantemEstado();
    void perfilInvalidoRecusado();
    void ocultaSolidoDuranteEdicaoDeEsboco();
    void selecaoNoSolidoExtrudado();
    void navegacaoEViewCube();
    void salvarEAbrir();
    void exportarStep();
    void exportarStl();
    void dialogoStlReal();
    void exportarStlPeloDialogoReal();
    void fechamento();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at(double u, double v) const
    {
        const SketchPlane& plane = m_window->editingSketch()->plane();
        return toLogical(viewer(), project(viewer(), plane.toGlobal(u, v)));
    }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    // Esboço XY com retângulo 100 x 60 na origem (criado pelo mouse e ajustado por dimensões).
    void createRectangleSketch();
    // Esboço + extrusão de 20 mm pelo comando (gancho).
    void createExtrudedBlock();

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestExtrusaoUi::init()
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

void TestExtrusaoUi::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestExtrusaoUi::createRectangleSketch()
{
    m_window->nextPlane = PlaneType::XY;
    action("actionNovoEsboco")->trigger();
    QVERIFY(m_window->isEditingSketch());
    action("actionFerramentaRetangulo")->trigger();
    QVERIFY(click(viewer(), at(10.0, 10.0)));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at(60.0, 40.0)));
    pauseBetweenClicks();
    action("actionFerramentaSelecionar")->trigger();
    QCOMPARE(m_window->editingSketch()->entities().size(), size_t(1));
    QVERIFY(m_window->updateSketchEntity("e-1", SketchRectangle{0.0, 0.0, 100.0, 60.0}));
    QVERIFY(m_window->finishSketch());
    QCOMPARE(m_window->selectedSketchId(), QStringLiteral("sk-1")); // esboço concluído fica selecionado
}

void TestExtrusaoUi::createExtrudedBlock()
{
    createRectangleSketch();
    m_window->nextExtrusion = ExtrudeParameters{"", "sk-1", 20.0, false};
    action("actionExtrusao")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(1));
}

void TestExtrusaoUi::documentoInicialVazio()
{
    // 1. O aplicativo inicia com documento vazio; a peça de teste não é aberta.
    QVERIFY(m_window->document().operations().empty());
    QVERIFY(m_window->document().sketches().empty());
    QVERIFY(m_window->part().IsNull());
    QVERIFY(!m_window->document().hasPart());
    QCOMPARE(m_window->operationsTree()->topLevelItem(0)->childCount(), 0);
    QVERIFY(!action("actionExtrusao")->isEnabled());
    QVERIFY(!action("actionExportarStl")->isEnabled());
    QVERIFY(!action("actionExportarStep")->isEnabled());
    saveEvidence(m_window->grab().toImage(), "E53_01_documento_vazio");
}

void TestExtrusaoUi::comandoSemEsbocoExplica()
{
    QVERIFY(!action("actionExtrusao")->isEnabled());
    QVERIFY(action("actionExtrusao")->toolTip().contains(QStringLiteral("esboço")));
    // Acionado mesmo assim (atalho/programático): explica e não altera o documento.
    m_window->extrude();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("Selecione")));
    QVERIFY(m_window->document().operations().empty());
    // Exportação STL sem sólido: mensagem clara.
    m_window->exportStl();
    QCOMPARE(m_window->errors.size(), 2);
    QVERIFY(m_window->errors.last().contains(QStringLiteral("não contém sólido")));
}

void TestExtrusaoUi::esbocoPeloMouseEExtrusao()
{
    createRectangleSketch();
    QVERIFY(action("actionExtrusao")->isEnabled());
    m_window->nextExtrusion = ExtrudeParameters{"", "sk-1", 20.0, false};
    action("actionExtrusao")->trigger();
    // Valores iniciais propostos pelo comando.
    QCOMPARE(m_window->lastInitial.sketchId, std::string("sk-1"));
    QCOMPARE(m_window->lastInitial.name, std::string("Extrusão 1"));
    QCOMPARE(m_window->lastInitial.depth, 10.0);
    QVERIFY(m_window->errors.isEmpty());
    const auto& ops = m_window->document().operations();
    QCOMPARE(ops.size(), size_t(1));
    QCOMPARE(ops[0].id, std::string("op-1"));
    QCOMPARE(ops[0].name, std::string("Extrusão 1"));
    const auto info = ShapeInspector::inspect(m_window->part());
    QVERIFY(info.isValid);
    QCOMPARE(info.solids, 1);
    QCOMPARE(info.faces, 6);
    QVERIFY(std::fabs(info.volume - kBoxVolume) < 1e-6);
    QVERIFY(m_window->document().isModified());
    QCOMPARE(m_window->selectedOperationId(), QStringLiteral("op-1"));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("criada")));
    QVERIFY(action("actionExportarStl")->isEnabled());
    QVERIFY(action("actionEditarOperacao")->isEnabled());
}

void TestExtrusaoUi::cancelarNaoAltera()
{
    createRectangleSketch();
    const int revision = m_window->document().revision();
    m_window->nextExtrusion.reset(); // diálogo cancelado
    action("actionExtrusao")->trigger();
    QVERIFY(m_window->document().operations().empty());
    QCOMPARE(m_window->document().revision(), revision);
    QVERIFY(m_window->part().IsNull());
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("cancelada")));

    // Diálogo real cancelado: pré-visualização removida, documento intacto.
    m_window->useRealExtrusionDialog = true;
    m_window->selectTreeItem("sk-1");
    bool sawPreview = false;
    whenDialogOpens<cad::app::ExtrusionDialog>([&](cad::app::ExtrusionDialog* d) {
        sawPreview = viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation());
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Cancel)->click();
    });
    action("actionExtrusao")->trigger();
    QVERIFY(sawPreview);
    QVERIFY(m_window->document().operations().empty());
    QCOMPARE(m_window->document().revision(), revision);
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
}

void TestExtrusaoUi::dialogoRealComPreVisualizacao()
{
    createRectangleSketch();
    m_window->useRealExtrusionDialog = true;
    bool checked = false;
    whenDialogOpens<cad::app::ExtrusionDialog>([&](cad::app::ExtrusionDialog* d) {
        auto* depth = d->findChild<QDoubleSpinBox*>("campoProfundidade");
        auto* reverse = d->findChild<QCheckBox*>("opcaoInverter");
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        auto* origin = d->findChild<QLabel*>("rotuloEsbocoOrigem");
        auto* ok = d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok);
        QVERIFY(depth && reverse && message && origin && ok);
        QCOMPARE(depth->suffix(), QStringLiteral(" mm"));
        QVERIFY(origin->text().contains("sk-1") && origin->text().contains("XY"));
        QCOMPARE(ok->text(), QStringLiteral("Extrudar"));
        // Pré-visualização exibida; documento ainda sem operações.
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
        QVERIFY(m_window->document().operations().empty());
        QVERIFY(message->text().contains("60.000,00")); // 100 x 60 x 10
        depth->setValue(25.0);
        QVERIFY(message->text().contains("150.000,00"));
        QVERIFY(waitFrames(viewer()));
        saveEvidence(m_window->grab().toImage(), "E53_02_previsualizacao");
        saveEvidence(d->grab().toImage(), "E53_03_dialogo_extrusao");
        // Profundidade inválida: o diálogo permanece aberto com a mensagem; sem pré-visualização.
        depth->setValue(0.0);
        ok->click();
        QVERIFY(d->isVisible());
        QVERIFY(message->text().contains(QStringLiteral("maior que zero")));
        QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
        QVERIFY(m_window->document().operations().empty());
        depth->setValue(25.0);
        reverse->setChecked(true);
        checked = true;
        ok->click();
    });
    action("actionExtrusao")->trigger();
    QVERIFY(checked);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    const auto p = m_window->document().extrudeParameters("op-1");
    QCOMPARE(p.depth, 25.0);
    QVERIFY(p.reversed);
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->previewPresentation()));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - 150000.0) < 1e-6);
    // Sentido invertido: z em [-25, 0].
    double x0, y0, z0, x1, y1, z1;
    ShapeInspector::boundingBox(m_window->part()).Get(x0, y0, z0, x1, y1, z1);
    QVERIFY(std::fabs(z0 + 25.0) < 1e-3 && std::fabs(z1) < 1e-3);
}

void TestExtrusaoUi::arvoreDaExtrusao()
{
    createExtrudedBlock();
    QTreeWidgetItem* root = m_window->operationsTree()->topLevelItem(0);
    QCOMPARE(root->childCount(), 2); // esboço + extrusão
    QVERIFY(topItem(m_window->operationsTree(), "esboco", "sk-1"));
    QTreeWidgetItem* ext = topItem(m_window->operationsTree(), "operacao", "op-1");
    QVERIFY(ext);
    QVERIFY(m_window->operationsTree()->indexOfTopLevelItem(root) == 0);
    QVERIFY(root->indexOfChild(ext) > root->indexOfChild(topItem(m_window->operationsTree(), "esboco", "sk-1")));
    QVERIFY(ext->text(0).contains(QStringLiteral("Extrusão 1")));
    const QStringList children = childTexts(ext);
    qInfo("%s", qPrintable(children.join(" | ")));
    QVERIFY(children.size() >= 4);
    QVERIFY(children[0].startsWith(QStringLiteral("Esboço de origem: Esboço 1 (sk-1)")));
    QVERIFY(children[1].startsWith(QStringLiteral("Profundidade: 20,00 mm")));
    QVERIFY(children[2].startsWith(QStringLiteral("Sentido: normal do plano")));
    QVERIFY(children[3].contains(QStringLiteral("Estado:")));
    auto* panel = m_window->findChild<QLabel*>("painelPropriedades");
    QVERIFY(panel->text().contains("120.000,00"));
    saveEvidence(m_window->grab().toImage(), "E53_04_arvore_extrusao");
}

void TestExtrusaoUi::visualizadorExibeSolido()
{
    const QImage background = backgroundReference(viewer());
    createExtrudedBlock();
    QVERIFY(viewer()->shape().IsSame(m_window->part()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    saveEvidence(m_window->grab().toImage(), "E53_05_solido_extrudado");
    // Centro da face superior desenhado (difere do fundo); ponto longe do sólido continua fundo.
    const QPoint top = project(viewer(), gp_Pnt(50.0, 30.0, 20.0));
    QVERIFY2(differs(pixelAt(img, top), pixelAt(background, top), 20), "sólido não desenhado");
    const QPoint empty(8, img.height() / 2);
    QVERIFY(!differs(pixelAt(img, empty), pixelAt(background, empty), 20));
}

void TestExtrusaoUi::editarProfundidade()
{
    createExtrudedBlock();
    m_window->selectTreeItem("op-1");
    QVERIFY(action("actionEditarOperacao")->isEnabled());
    m_window->nextExtrusion = ExtrudeParameters{"Bloco extrudado", "sk-1", 35.0, false};
    action("actionEditarOperacao")->trigger();
    QCOMPARE(m_window->lastInitial.depth, 20.0); // diálogo recebe os parâmetros atuais
    QVERIFY(m_window->errors.isEmpty());
    QCOMPARE(m_window->document().extrudeParameters("op-1").depth, 35.0);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - 210000.0) < 1e-6);
    QTreeWidgetItem* ext = topItem(m_window->operationsTree(), "operacao", "op-1");
    QVERIFY(ext->text(0).contains("Bloco extrudado"));
    QVERIFY(childTexts(ext)[1].startsWith(QStringLiteral("Profundidade: 35,00 mm")));
    QVERIFY(viewer()->shape().IsSame(m_window->part()));

    // Profundidade inválida: recusada, último estado válido mantido.
    const TopoDS_Shape before = m_window->part();
    m_window->nextExtrusion = ExtrudeParameters{"Bloco extrudado", "sk-1", -5.0, false};
    action("actionEditarOperacao")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->part().IsSame(before));
    QCOMPARE(m_window->document().extrudeParameters("op-1").depth, 35.0);

    // Duplo clique na operação abre a edição.
    m_window->nextExtrusion = ExtrudeParameters{"Bloco extrudado", "sk-1", 12.5, false};
    emit m_window->operationsTree()->itemDoubleClicked(topItem(m_window->operationsTree(), "operacao", "op-1"), 0);
    QCOMPARE(m_window->document().extrudeParameters("op-1").depth, 12.5);
    saveEvidence(m_window->grab().toImage(), "E53_06_profundidade_editada");
}

void TestExtrusaoUi::editarEsbocoRecalcula()
{
    createExtrudedBlock();
    QVERIFY(m_window->editSketch("sk-1"));
    QVERIFY(m_window->updateSketchEntity("e-1", SketchRectangle{0.0, 0.0, 80.0, 60.0}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->errors.isEmpty());
    // Dependente recalculado na ordem de dependência.
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - 80.0 * 60.0 * 20.0) < 1e-6);
    QVERIFY(viewer()->shape().IsSame(m_window->part()));
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
}

void TestExtrusaoUi::edicaoDeEsbocoInvalidaMantemEstado()
{
    createExtrudedBlock();
    const TopoDS_Shape before = m_window->part();
    QVERIFY(m_window->editSketch("sk-1"));
    // Acrescenta um segundo contorno (círculo): o perfil fica ambíguo para a extrusão dependente.
    QVERIFY(m_window->addSketchGeometry(SketchCircle{150.0, 30.0, 10.0}));
    QVERIFY(!m_window->finishSketch());
    QCOMPARE(m_window->errors.size(), 1);
    qInfo("%s", qPrintable(m_window->errors.first()));
    QVERIFY(m_window->errors.first().contains(QStringLiteral("extrusão dependente")));
    QVERIFY(m_window->isEditingSketch()); // edição permanece aberta para correção
    QVERIFY(m_window->part().IsSame(before));
    QCOMPARE(m_window->document().findSketch("sk-1")->entities().size(), size_t(1));
    action("actionCancelarEsboco")->trigger();
    QVERIFY(!m_window->isEditingSketch());
    QVERIFY(m_window->part().IsSame(before));
    QVERIFY(viewer()->shape().IsSame(before));
}

void TestExtrusaoUi::perfilInvalidoRecusado()
{
    // Esboço com linha aberta: o comando explica o motivo e não abre o diálogo.
    m_window->nextPlane = PlaneType::XZ;
    action("actionNovoEsboco")->trigger();
    QVERIFY(m_window->addSketchGeometry(SketchLine{0.0, 0.0, 40.0, 0.0}));
    QVERIFY(m_window->addSketchGeometry(SketchLine{40.0, 0.0, 40.0, 30.0}));
    QVERIFY(m_window->finishSketch());
    m_window->nextExtrusion = ExtrudeParameters{"", "sk-1", 10.0, false};
    m_window->lastInitial = {};
    action("actionExtrusao")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("Perfil aberto")));
    QVERIFY(m_window->lastInitial.sketchId.empty()); // diálogo não foi aberto
    QVERIFY(m_window->document().operations().empty());
}

void TestExtrusaoUi::ocultaSolidoDuranteEdicaoDeEsboco()
{
    createExtrudedBlock();
    m_window->nextPlane = PlaneType::YZ;
    action("actionNovoEsboco")->trigger();
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(!action("actionExtrusao")->isEnabled());
    QVERIFY(!action("actionEditarOperacao")->isEnabled());
    action("actionCancelarEsboco")->trigger();
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    // Edição do esboço de origem também oculta e restaura.
    QVERIFY(m_window->editSketch("sk-1"));
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(m_window->finishSketch());
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
}

void TestExtrusaoUi::selecaoNoSolidoExtrudado()
{
    createExtrudedBlock();
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    auto message = [this]() { return m_window->statusBar()->currentMessage(); };
    QVERIFY(click(viewer(), at3d(50.0, 30.0, 20.0)));
    QTRY_VERIFY(message().startsWith("Face"));
    QVERIFY2(message().contains(QStringLiteral("Área: 6.000,00 mm²")), qPrintable(message()));
    saveEvidence(m_window->grab().toImage(), "E53_07_selecao_face");
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(50.0, 0.0, 20.0)));
    QTRY_VERIFY(message().startsWith("Aresta"));
    QVERIFY2(message().contains(QStringLiteral("Comprimento: 100,00 mm")), qPrintable(message()));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(100.0, 0.0, 20.0)));
    QTRY_VERIFY(message().startsWith(QString::fromUtf8("Vértice")));
    QVERIFY2(message().contains("X 100,00") && message().contains("Z 20,00"), qPrintable(message()));
    saveEvidence(m_window->grab().toImage(), "E53_08_selecao_vertice");
}

void TestExtrusaoUi::navegacaoEViewCube()
{
    createExtrudedBlock();
    action("actionVistaIsometrica")->trigger();
    const gp_Dir dir = viewer()->view()->Camera()->Direction();
    const QPointF c(viewer()->width() / 2.0, viewer()->height() / 2.0);
    QVERIFY(drag(viewer(), c, c + QPointF(100, 40), Qt::MiddleButton));
    QVERIFY(dir.Angle(viewer()->view()->Camera()->Direction()) > 0.1);
    const double scale = viewer()->view()->Camera()->Scale();
    sendWheel(viewer(), c, 120);
    QVERIFY(waitFrames(viewer()));
    QVERIFY(viewer()->view()->Camera()->Scale() < scale);
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->viewCube()));
    action("actionVistaFrente")->trigger();
    QVERIFY(viewer()->view()->Camera()->Direction().Angle(gp_Dir(0, 1, 0)) < 1e-3);
    // Clique real na face SUPERIOR do ViewCube (visível na vista isométrica).
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
    QTest::keyClick(viewer(), Qt::Key_F);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E53_09_vista_superior_viewcube");
}

void TestExtrusaoUi::salvarEAbrir()
{
    createExtrudedBlock();
    m_window->nextSavePath = m_dir->filePath("extrusao.cadmec");
    action("actionSalvar")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QVERIFY(!m_window->document().isModified());
    action("actionNovo")->trigger();
    QVERIFY(m_window->part().IsNull());
    m_window->nextOpenPath = m_window->nextSavePath;
    action("actionAbrir")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    QCOMPARE(m_window->document().extrudeParameters("op-1").depth, 20.0);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - kBoxVolume) < 1e-6);
    QVERIFY(topItem(m_window->operationsTree(), "operacao", "op-1"));
    QVERIFY(viewer()->shape().IsSame(m_window->part()));

    // Arquivo inválido: recusado sem perder o documento ativo.
    const QString bad = m_dir->filePath("invalido.cadmec");
    QFile f(bad);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"format\": \"CadMecanico\"");
    f.close();
    const TopoDS_Shape active = m_window->part();
    m_window->nextOpenPath = bad;
    action("actionAbrir")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->part().IsSame(active));
    QCOMPARE(m_window->document().operations().size(), size_t(1));
}

void TestExtrusaoUi::exportarStep()
{
    createExtrudedBlock();
    m_window->nextExportPath = m_dir->filePath("extrusao.step");
    action("actionExportarStep")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 6);
    QVERIFY(std::fabs(s.volume - kBoxVolume) < kBoxVolume * 1e-9);
}

void TestExtrusaoUi::exportarStl()
{
    createExtrudedBlock();
    const QString path = m_dir->filePath("extrusao.stl");
    cad::io::StlExportOptions options;
    m_window->nextStl = MainWindow::StlExportRequest{path, options};
    action("actionExportarStl")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(m_window->infos.first().contains(QStringLiteral("binário")));
    QVERIFY(m_window->infos.first().contains(QStringLiteral("milímetros")));
    const auto mesh = cad::test::stl::read(path.toStdString());
    QVERIFY2(mesh.ok, mesh.error.c_str());
    QVERIFY(mesh.binary);
    QCOMPARE(int(mesh.triangles.size()), 12);
    QVERIFY(std::fabs(mesh.volume() - kBoxVolume) < 1e-3);
    QVERIFY(std::fabs(mesh.max.x - 100.0) < 1e-4 && std::fabs(mesh.max.y - 60.0) < 1e-4
            && std::fabs(mesh.max.z - 20.0) < 1e-4);
    QVERIFY(std::fabs(mesh.min.x) < 1e-4 && std::fabs(mesh.min.y) < 1e-4 && std::fabs(mesh.min.z) < 1e-4);

    // Sobrescrita recusada: arquivo existente mantido intacto.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    m_window->overwrite = false;
    m_window->nextStl = MainWindow::StlExportRequest{path, cad::io::StlExportOptions{cad::io::StlFormat::Ascii}};
    action("actionExportarStl")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();

    // Cancelamento do diálogo não grava nada.
    m_window->nextStl.reset();
    action("actionExportarStl")->trigger();
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("cancelada")));
    QVERIFY(!QFile::exists(m_dir->filePath("extrusao (2).stl")));
}

void TestExtrusaoUi::dialogoStlReal()
{
    createExtrudedBlock();
    cad::app::StlExportDialog dialog(m_dir->filePath("peca"), m_window->part());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* binary = dialog.findChild<QRadioButton*>("opcaoBinario");
    auto* quality = dialog.findChild<QComboBox*>("campoQualidade");
    auto* custom = dialog.findChild<QCheckBox*>("opcaoTolerancias");
    auto* linear = dialog.findChild<QDoubleSpinBox*>("campoTolLinear");
    auto* params = dialog.findChild<QLabel*>("rotuloParametrosMalha");
    auto* units = dialog.findChild<QLabel*>("avisoUnidadeStl");
    auto* buttons = dialog.findChild<QDialogButtonBox*>("botoesExportarStl");
    QVERIFY(binary && quality && custom && linear && params && units && buttons);
    QVERIFY(binary->isChecked());
    QCOMPARE(quality->currentText(), QStringLiteral("Média"));
    QVERIFY(!linear->isEnabled());
    QVERIFY(units->text().contains(QStringLiteral("milímetros")));
    QVERIFY(params->text().contains(QStringLiteral("deflexão linear")));
    QVERIFY(dialog.filePath().endsWith("peca.stl"));
    saveEvidence(dialog.grab().toImage(), "E53_10_dialogo_stl");
    // Tolerância personalizada inválida: diálogo permanece aberto com mensagem.
    custom->setChecked(true);
    QVERIFY(linear->isEnabled());
    linear->setValue(0.0);
    buttons->button(QDialogButtonBox::Ok)->click();
    QVERIFY(dialog.isVisible());
    QVERIFY(!dialog.validationMessage().isEmpty());
    linear->setValue(0.05);
    quality->setCurrentIndex(2);
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto o = dialog.options();
    QVERIFY(o.format == cad::io::StlFormat::Binary);
    QVERIFY(o.quality == cad::io::StlQuality::High);
    QVERIFY(o.linearDeflection && *o.linearDeflection == 0.05);
}

void TestExtrusaoUi::exportarStlPeloDialogoReal()
{
    createExtrudedBlock();
    m_window->useRealStlDialog = true;
    const QString path = m_dir->filePath("dialogo_real_ascii.stl");
    whenDialogOpens<cad::app::StlExportDialog>([&](cad::app::StlExportDialog* d) {
        d->findChild<QLineEdit*>("campoCaminhoStl")->setText(QDir::toNativeSeparators(path));
        d->findChild<QRadioButton*>("opcaoAscii")->setChecked(true);
        d->findChild<QDialogButtonBox*>("botoesExportarStl")->button(QDialogButtonBox::Ok)->click();
    });
    action("actionExportarStl")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->infos.size(), 1);
    const auto mesh = cad::test::stl::read(path.toStdString());
    QVERIFY2(mesh.ok, mesh.error.c_str());
    QVERIFY(!mesh.binary);
    QVERIFY(std::fabs(mesh.volume() - kBoxVolume) < 1e-2);
    // O B-Rep do documento não recebe malha.
    QVERIFY(ShapeInspector::inspect(m_window->part()).isValid);
}

void TestExtrusaoUi::fechamento()
{
    createExtrudedBlock();
    QVERIFY(m_window->close());
    QVERIFY(!m_window->isVisible());
    delete m_window;
    m_window = nullptr;
}

QTEST_MAIN(TestExtrusaoUi)
#include "test_extrusao_ui.moc"
