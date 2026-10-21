// Operações booleanas na interface real (E5.4), com contexto OpenGL real: comandos do menu
// Operações, diálogo com destaque dos operandos (A azul, B vermelho), cancelamento, árvore,
// ocultação dos operandos consumidos, furo passante visível, edição e recálculo, seleção,
// navegação, salvar/abrir e exportação STEP/STL sem duplicação.
#include "StlTestUtils.h"
#include "ViewerTestUtils.h"

#include "app/BooleanDialog.h"
#include "app/MainWindow.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <AIS_ViewCube.hxx>
#include <Graphic3d_Camera.hxx>

#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>

using cad::app::MainWindow;
using cad::document::BooleanParameters;
using cad::modeling::BooleanKind;
using cad::modeling::ShapeInspector;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBox = 100.0 * 60.0 * 20.0;
const double kHoled = kBox - 2000.0 * kPi;

class TestableMainWindow : public MainWindow
{
public:
    std::optional<BooleanParameters> nextBoolean;
    bool useRealBooleanDialog = false;
    BooleanParameters lastInitial;
    QString lastEditedId;
    std::optional<StlExportRequest> nextStl;
    QString nextSavePath;
    QString nextOpenPath;
    QString nextExportPath;
    std::optional<PlaneType> nextPlane = PlaneType::XY;
    QStringList errors;
    QStringList infos;

protected:
    std::optional<PlaneType> askSketchPlane() override { return nextPlane; }
    std::optional<BooleanParameters> askBoolean(const BooleanParameters& initial, const QString& id) override
    {
        lastInitial = initial;
        lastEditedId = id;
        if (useRealBooleanDialog)
            return MainWindow::askBoolean(initial, id);
        return nextBoolean;
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

QTreeWidgetItem* topItem(QTreeWidget* tree, const QString& id)
{
    QTreeWidgetItem* root = tree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
        if (root->child(i)->data(0, MainWindow::kItemIdRole).toString() == id
            && root->child(i)->data(0, MainWindow::kItemKindRole).toString() == QLatin1String("operacao"))
            return root->child(i);
    return nullptr;
}

QStringList childTexts(const QTreeWidgetItem* item)
{
    QStringList texts;
    for (int i = 0; item && i < item->childCount(); ++i)
        texts << item->child(i)->text(0);
    return texts;
}

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

} // namespace

class TestBooleanasUi : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void comandoIndisponivelSemDoisCorpos();
    void subtracaoPeloMenu();
    void furoPassanteVisivel();
    void dialogoRealDestacaOperandos();
    void dialogoRealCriaIntersecao();
    void cancelamentoNaoAltera();
    void edicaoERecalculo();
    void intersecaoVaziaInformada();
    void uniaoDisjuntaInformaComposto();
    void selecaoNoResultado();
    void navegacaoEViewCube();
    void ocultacaoDuranteEsboco();
    void salvarAbrirEExportar();
    void fechamento();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    // Esboço com uma entidade + extrusão (pela API da janela). Retorna o id da extrusão.
    QString body(const char* name, PlaneType plane, const SketchGeometry& g, double depth);
    // Bloco 100 x 60 x 20 (z em [5, 25], y em [-60, 0]) e cilindro R10 z em [0, 30] que o atravessa.
    void blockAndCylinder();

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestBooleanasUi::init()
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

void TestBooleanasUi::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

QString TestBooleanasUi::body(const char* name, PlaneType plane, const SketchGeometry& g, double depth)
{
    if (!m_window->startNewSketch(plane) || !m_window->addSketchGeometry(g) || !m_window->finishSketch())
        return QString();
    const QString sketch = m_window->selectedSketchId();
    if (!m_window->addExtrusion({name, sketch.toStdString(), depth, false}))
        return QString();
    return m_window->selectedOperationId();
}

void TestBooleanasUi::blockAndCylinder()
{
    QCOMPARE(body("Bloco", PlaneType::XZ, SketchRectangle{0.0, 5.0, 100.0, 20.0}, 60.0), QStringLiteral("op-1"));
    QCOMPARE(body("Cilindro", PlaneType::XY, SketchCircle{50.0, -30.0, 10.0}, 30.0), QStringLiteral("op-2"));
    QVERIFY(m_window->errors.isEmpty());
    QCOMPARE(m_window->document().solidCount(), 2);
}

void TestBooleanasUi::comandoIndisponivelSemDoisCorpos()
{
    for (const char* name : {"actionUniao", "actionSubtracao", "actionIntersecao"})
    {
        QVERIFY(action(name));
        QVERIFY(!action(name)->isEnabled());
        QVERIFY(action(name)->toolTip().contains(QStringLiteral("dois corpos")));
    }
    // Acionado mesmo assim: explicação, documento intacto.
    m_window->booleanCommand(BooleanKind::Cut);
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("dois corpos")));
    QVERIFY(m_window->document().operations().empty());
    QVERIFY(!body("Bloco", PlaneType::XY, SketchRectangle{0, 0, 10, 10}, 5).isEmpty());
    QVERIFY(!action("actionSubtracao")->isEnabled()); // um corpo só
}

void TestBooleanasUi::subtracaoPeloMenu()
{
    blockAndCylinder();
    QVERIFY(action("actionSubtracao")->isEnabled());
    m_window->selectTreeItem("op-1");
    m_window->nextBoolean = BooleanParameters{"", BooleanKind::Cut, "op-1", "op-2"};
    action("actionSubtracao")->trigger();
    // Valores propostos: tipo do comando, A = corpo selecionado, B = outro corpo ativo.
    QVERIFY(m_window->lastInitial.kind == BooleanKind::Cut);
    QCOMPARE(m_window->lastInitial.operandA, std::string("op-1"));
    QCOMPARE(m_window->lastInitial.operandB, std::string("op-2"));
    QCOMPARE(m_window->lastInitial.name, std::string("Subtração 1"));
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));

    const auto& doc = m_window->document();
    QCOMPARE(doc.operations().size(), size_t(3));
    QVERIFY(std::fabs(ShapeInspector::volume(doc.shape()) - kHoled) < 1e-6);
    QCOMPARE(doc.solidCount(), 1);
    QVERIFY(viewer()->shape().IsSame(doc.shape())); // só o resultado é exibido
    QCOMPARE(m_window->selectedOperationId(), QStringLiteral("op-3"));
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("1 sólido")));

    // Árvore: operandos preservados e marcados; operação com tipo, A, B, resultado e estado.
    QTreeWidgetItem* cut = topItem(m_window->operationsTree(), "op-3");
    QVERIFY(cut);
    QCOMPARE(cut->text(0), QStringLiteral("Subtração 1 — Bloco − Cilindro"));
    const QStringList children = childTexts(cut);
    qInfo("%s", qPrintable(children.join(" | ")));
    QCOMPARE(children.size(), 5);
    QCOMPARE(children[0], QStringLiteral("Tipo: Subtração (A − B)"));
    QCOMPARE(children[1], QStringLiteral("Corpo principal (A): Bloco (op-1)"));
    QCOMPARE(children[2], QStringLiteral("Ferramenta de corte (B): Cilindro (op-2)"));
    QVERIFY(children[3].startsWith(QStringLiteral("Resultado: 1 sólido, volume 113.716,81 mm³")));
    QCOMPARE(children[4], QStringLiteral("Estado: Válida"));
    for (const char* id : {"op-1", "op-2"})
        QVERIFY(topItem(m_window->operationsTree(), id)->text(0).contains(QStringLiteral("consumido por Subtração 1")));
    QTreeWidgetItem* root = m_window->operationsTree()->topLevelItem(0);
    QVERIFY(root->indexOfChild(cut) > root->indexOfChild(topItem(m_window->operationsTree(), "op-2")));
    auto* panel = m_window->findChild<QLabel*>("painelPropriedades");
    QVERIFY(panel->text().contains(QStringLiteral("113.716,81")));
    m_window->selectTreeItem("op-1");
    QVERIFY(panel->text().contains(QStringLiteral("Consumido por Subtração 1")));
    // Nenhum outro corpo ativo: booleanas indisponíveis.
    QVERIFY(!action("actionUniao")->isEnabled());
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E54_01_subtracao_arvore");
}

void TestBooleanasUi::furoPassanteVisivel()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    action("actionVistaSuperior")->trigger();
    QVERIFY(waitFrames(viewer()));
    const QImage img = viewer()->grabViewerImage();
    const QImage background = backgroundReference(viewer()); // mesmo tamanho e câmera, cena vazia
    saveEvidence(m_window->grab().toImage(), "E54_02_furo_passante_vista_superior");
    // Centro do furo: vê-se o fundo através da peça; ao lado do furo, material.
    const QPoint hole = project(viewer(), gp_Pnt(50.0, -30.0, 25.0));
    const QPoint material = project(viewer(), gp_Pnt(50.0, -10.0, 25.0));
    QVERIFY2(!differs(pixelAt(img, hole), pixelAt(background, hole), 20), "furo não atravessa a peça");
    QVERIFY2(differs(pixelAt(img, material), pixelAt(background, material), 20), "material ausente");
}

void TestBooleanasUi::dialogoRealDestacaOperandos()
{
    blockAndCylinder();
    m_window->useRealBooleanDialog = true;
    m_window->selectTreeItem("op-1");
    bool checked = false;
    whenDialogOpens<cad::app::BooleanDialog>([&](cad::app::BooleanDialog* d) {
        auto* kind = d->findChild<QComboBox*>("campoTipoBooleana");
        auto* a = d->findChild<QComboBox*>("campoCorpoA");
        auto* b = d->findChild<QComboBox*>("campoCorpoB");
        auto* labelA = d->findChild<QLabel*>("rotuloCorpoA");
        auto* labelB = d->findChild<QLabel*>("rotuloCorpoB");
        auto* message = d->findChild<QLabel*>("mensagemBooleana");
        auto* ok = d->findChild<QDialogButtonBox*>("botoesBooleana")->button(QDialogButtonBox::Ok);
        QVERIFY(kind && a && b && labelA && labelB && message && ok);
        QCOMPARE(a->count(), 2);
        QCOMPARE(a->currentText(), QStringLiteral("Bloco (op-1)"));
        QCOMPARE(b->currentText(), QStringLiteral("Cilindro (op-2)"));
        QVERIFY(labelA->text().contains(QStringLiteral("Corpo principal (A)")));
        QVERIFY(labelB->text().contains(QStringLiteral("Ferramenta de corte (B)")));
        QVERIFY2(message->text().contains(QStringLiteral("113.716,81")), qPrintable(message->text()));
        // Destaque dos operandos no visualizador; documento ainda sem a operação.
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->operandAPresentation()));
        QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->operandBPresentation()));
        QVERIFY(viewer()->operandAPresentation()->Shape().IsSame(m_window->document().findOperation("op-1")->result));
        QVERIFY(viewer()->operandBPresentation()->Shape().IsSame(m_window->document().findOperation("op-2")->result));
        QCOMPARE(m_window->document().operations().size(), size_t(2));
        action("actionVistaIsometrica")->trigger();
        QVERIFY(waitFrames(viewer()));
        saveEvidence(m_window->grab().toImage(), "E54_03_destaque_operandos");
        saveEvidence(d->grab().toImage(), "E54_04_dialogo_subtracao");
        // Troca de tipo: rótulos e resultado acompanham.
        kind->setCurrentIndex(0);
        QVERIFY(labelA->text().contains(QStringLiteral("Corpo A")));
        QVERIFY(message->text().contains(QStringLiteral("123.141,59")));
        // Seleção inválida (A = B): mensagem e diálogo permanece aberto.
        b->setCurrentIndex(a->currentIndex());
        QVERIFY(message->text().contains(QStringLiteral("diferentes")));
        ok->click();
        QVERIFY(d->isVisible());
        QCOMPARE(m_window->document().operations().size(), size_t(2));
        checked = true;
        d->reject();
    });
    action("actionSubtracao")->trigger();
    QVERIFY(checked);
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(viewer()->operandAPresentation().IsNull() && viewer()->operandBPresentation().IsNull());
}

void TestBooleanasUi::dialogoRealCriaIntersecao()
{
    blockAndCylinder();
    m_window->useRealBooleanDialog = true;
    whenDialogOpens<cad::app::BooleanDialog>([&](cad::app::BooleanDialog* d) {
        auto* b = d->findChild<QComboBox*>("campoCorpoB");
        auto* a = d->findChild<QComboBox*>("campoCorpoA");
        a->setCurrentIndex(a->findData(QStringLiteral("op-1")));
        b->setCurrentIndex(b->findData(QStringLiteral("op-2")));
        QCOMPARE(d->findChild<QComboBox*>("campoTipoBooleana")->currentIndex(), 2);
        d->findChild<QDialogButtonBox*>("botoesBooleana")->button(QDialogButtonBox::Ok)->click();
    });
    action("actionIntersecao")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(3));
    QCOMPARE(m_window->document().operations()[2].name, std::string("Interseção 1"));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - 2000.0 * kPi) < 1e-6);
    QVERIFY(viewer()->operandAPresentation().IsNull());
}

void TestBooleanasUi::cancelamentoNaoAltera()
{
    blockAndCylinder();
    const int revision = m_window->document().revision();
    const TopoDS_Shape before = m_window->part();
    m_window->nextBoolean.reset();
    action("actionUniao")->trigger();
    QCOMPARE(m_window->document().revision(), revision);
    QVERIFY(m_window->part().IsSame(before));
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(m_window->document().consumerOf("op-1").empty());
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("cancelada")));
}

void TestBooleanasUi::edicaoERecalculo()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    m_window->selectTreeItem("op-3");
    QVERIFY(action("actionEditarOperacao")->isEnabled());
    m_window->nextBoolean = BooleanParameters{"Bloco ∪ cilindro", BooleanKind::Fuse, "op-1", "op-2"};
    action("actionEditarOperacao")->trigger();
    QCOMPARE(m_window->lastEditedId, QStringLiteral("op-3"));
    QVERIFY(m_window->lastInitial.kind == BooleanKind::Cut);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox + 1000.0 * kPi)) < 1e-6);
    QVERIFY(topItem(m_window->operationsTree(), "op-3")->text(0).startsWith(QStringLiteral("Bloco ∪ cilindro — Bloco ∪ Cilindro")));
    // Volta à subtração pelo diálogo real em modo de edição (operandos consumidos disponíveis).
    m_window->useRealBooleanDialog = true;
    whenDialogOpens<cad::app::BooleanDialog>([&](cad::app::BooleanDialog* d) {
        QCOMPARE(d->findChild<QComboBox*>("campoCorpoA")->count(), 2);
        d->findChild<QComboBox*>("campoTipoBooleana")->setCurrentIndex(1);
        QCOMPARE(d->findChild<QDialogButtonBox*>("botoesBooleana")->button(QDialogButtonBox::Ok)->text(),
                 QStringLiteral("Aplicar"));
        d->findChild<QDialogButtonBox*>("botoesBooleana")->button(QDialogButtonBox::Ok)->click();
    });
    emit m_window->operationsTree()->itemDoubleClicked(topItem(m_window->operationsTree(), "op-3"), 0);
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - kHoled) < 1e-6);
    m_window->useRealBooleanDialog = false;

    // Raio do cilindro alterado no esboço: o corte é recalculado.
    QVERIFY(m_window->editSketch("sk-2"));
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(m_window->updateSketchEntity("e-1", SketchCircle{50.0, -30.0, 15.0}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - (kBox - 225.0 * kPi * 20.0)) < 1e-6);
    QVERIFY(viewer()->shape().IsSame(m_window->part()));
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "op-3"))[3].contains(QStringLiteral("105.862,83")));
    saveEvidence(m_window->grab().toImage(), "E54_05_recalculo_raio_15");

    // Edição inválida (corpo B = corpo A): recusada, último estado mantido.
    const TopoDS_Shape before = m_window->part();
    m_window->selectTreeItem("op-3");
    m_window->nextBoolean = BooleanParameters{"", BooleanKind::Cut, "op-1", "op-1"};
    action("actionEditarOperacao")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->part().IsSame(before));
}

void TestBooleanasUi::intersecaoVaziaInformada()
{
    QVERIFY(!body("A", PlaneType::XY, SketchRectangle{0, 0, 100, 60}, 20).isEmpty());
    QVERIFY(!body("B", PlaneType::XY, SketchRectangle{200, 0, 100, 60}, 20).isEmpty());
    const TopoDS_Shape before = m_window->part();
    m_window->nextBoolean = BooleanParameters{"", BooleanKind::Common, "op-1", "op-2"};
    action("actionIntersecao")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains(QStringLiteral("Interseção vazia")));
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    QVERIFY(m_window->part().IsSame(before));
    QVERIFY(viewer()->shape().IsSame(before)); // nada de geometria antiga como resultado
}

void TestBooleanasUi::uniaoDisjuntaInformaComposto()
{
    QVERIFY(!body("A", PlaneType::XY, SketchRectangle{0, 0, 100, 60}, 20).isEmpty());
    QVERIFY(!body("B", PlaneType::XY, SketchRectangle{200, 0, 100, 60}, 20).isEmpty());
    m_window->nextBoolean = BooleanParameters{"", BooleanKind::Fuse, "op-1", "op-2"};
    action("actionUniao")->trigger();
    QVERIFY(m_window->statusBar()->currentMessage().contains(QStringLiteral("2 sólidos desconectados")));
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "op-3"))[3].contains(QStringLiteral("2 sólidos desconectados")));
    QCOMPARE(m_window->document().solidCount(), 2);
}

void TestBooleanasUi::selecaoNoResultado()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    auto message = [this]() { return m_window->statusBar()->currentMessage(); };
    QVERIFY(click(viewer(), at3d(20.0, -15.0, 25.0)));
    QTRY_VERIFY(message().startsWith("Face"));
    QVERIFY2(message().contains(QStringLiteral("Área: 5.685,84 mm²")), qPrintable(message())); // 6000 − 100π
    saveEvidence(m_window->grab().toImage(), "E54_06_selecao_face_furada");
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(50.0, -60.0, 25.0)));
    QTRY_VERIFY(message().startsWith("Aresta"));
    QVERIFY2(message().contains(QStringLiteral("Comprimento: 100,00 mm")), qPrintable(message()));
    pauseBetweenClicks();
    QVERIFY(click(viewer(), at3d(100.0, -60.0, 25.0)));
    QTRY_VERIFY(message().startsWith(QString::fromUtf8("Vértice")));
}

void TestBooleanasUi::navegacaoEViewCube()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    action("actionVistaIsometrica")->trigger();
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

void TestBooleanasUi::ocultacaoDuranteEsboco()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    QVERIFY(m_window->startNewSketch(PlaneType::YZ));
    QVERIFY(!viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(!action("actionSubtracao")->isEnabled());
    action("actionCancelarEsboco")->trigger();
    QVERIFY(viewer()->aisContext()->IsDisplayed(viewer()->shapePresentation()));
    QVERIFY(viewer()->shape().IsSame(m_window->part()));
}

void TestBooleanasUi::salvarAbrirEExportar()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    m_window->nextSavePath = m_dir->filePath("booleana.cadmec");
    action("actionSalvar")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    action("actionNovo")->trigger();
    QVERIFY(m_window->part().IsNull());
    m_window->nextOpenPath = m_window->nextSavePath;
    action("actionAbrir")->trigger();
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QCOMPARE(m_window->document().operations().size(), size_t(3));
    QCOMPARE(m_window->document().activeBodies(), std::vector<std::string>{"op-3"});
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->part()) - kHoled) < 1e-6);
    QVERIFY(topItem(m_window->operationsTree(), "op-3"));

    // STEP: somente o resultado ativo (operandos ocultos não são duplicados).
    m_window->nextExportPath = m_dir->filePath("booleana.step");
    action("actionExportarStep")->trigger();
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 7);
    QVERIFY(std::fabs(s.volume - kHoled) < kHoled * 1e-9);

    // STL binário e ASCII.
    for (auto format : {cad::io::StlFormat::Binary, cad::io::StlFormat::Ascii})
    {
        const QString path = m_dir->filePath(format == cad::io::StlFormat::Binary ? "b.stl" : "a.stl");
        cad::io::StlExportOptions o;
        o.format = format;
        o.quality = cad::io::StlQuality::High;
        m_window->nextStl = MainWindow::StlExportRequest{path, o};
        action("actionExportarStl")->trigger();
        const auto mesh = cad::test::stl::read(path.toStdString());
        QVERIFY2(mesh.ok, mesh.error.c_str());
        QCOMPARE(mesh.binary, format == cad::io::StlFormat::Binary);
        qInfo("STL %s: %d triangulos, volume %.3f", mesh.binary ? "binario" : "ASCII", int(mesh.triangles.size()),
              mesh.volume());
        QVERIFY(std::fabs(mesh.volume() - kHoled) / kHoled < 0.002);
        QVERIFY(std::fabs(mesh.min.z - 5.0) < 1e-4 && std::fabs(mesh.max.z - 25.0) < 1e-4);
        QCOMPARE(mesh.degenerate(), 0);
    }
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
}

void TestBooleanasUi::fechamento()
{
    blockAndCylinder();
    QVERIFY(m_window->addBoolean({"", BooleanKind::Cut, "op-1", "op-2"}));
    QVERIFY(m_window->close());
    QVERIFY(!m_window->isVisible());
    delete m_window;
    m_window = nullptr;
}

QTEST_MAIN(TestBooleanasUi)
#include "test_booleanas_ui.moc"
