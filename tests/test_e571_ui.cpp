// E5.7.1 na interface real, com contexto OpenGL real:
//  P5 — condições finais no diálogo único (disponibilidade por tipo de operação, profundidade,
//       cortar o lado de fora, direção 2), "Até a face" com a seleção REAL da face limite no
//       visualizador (o diálogo fecha, a face é clicada e o diálogo reabre) e reconstrução;
//  P3 — reconstrução da cadeia pela interface (esboço sobre a face lateral de uma saliência que
//       muda de largura) com a árvore e o diagnóstico coerentes.
#include "ViewerTestUtils.h"

#include "app/ExtrusionDialog.h"
#include "app/MainWindow.h"
#include "modeling/FaceNaming.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <BRepClass3d_SolidClassifier.hxx>
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
using cad::modeling::ShapeInspector;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

constexpr double kPi = 3.14159265358979323846;

class TestableMainWindow : public MainWindow
{
public:
    bool useRealExtrusionDialog = true;
    std::optional<ExtrudeParameters> nextExtrusion;
    // Seleção da face limite: clique simulado no ponto 3D dado (laço REAL de seleção).
    std::function<void()> clickLimitFace;
    int limitAsked = 0;
    QStringList errors, infos;

protected:
    std::optional<ExtrudeParameters> askExtrusion(const ExtrudeParameters& initial, bool editing) override
    {
        if (useRealExtrusionDialog)
            return MainWindow::askExtrusion(initial, editing);
        if (!nextExtrusion)
            return std::nullopt;
        ExtrudeParameters p = *nextExtrusion;
        p.sketchId = initial.sketchId;
        return p;
    }
    std::optional<std::pair<std::string, cad::modeling::FaceSignature>> askLimitFace() override
    {
        ++limitAsked;
        if (clickLimitFace)
            QTimer::singleShot(300, clickLimitFace);
        return MainWindow::askLimitFace();
    }
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

bool enabledItem(QComboBox* combo, EndCondition c)
{
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    return model && model->item(combo->findData(int(c)))->isEnabled();
}

} // namespace

class TestE571Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void condicoesNoDialogo();
    void direcao2PeloDialogo();
    void ateAFaceComSelecaoReal();
    void ateAFaceNaEdicao();
    void cadeiaPeloNomeTopologico();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    QPointF at3d(double x, double y, double z) const { return toLogical(viewer(), project(viewer(), gp_Pnt(x, y, z))); }
    void iso()
    {
        action("actionVistaIsometrica")->trigger();
        QVERIFY(waitFrames(viewer()));
    }
    // Bloco 100 × 60 × 20 e esboço com círculo R 5 em (20, 30) sobre a face superior.
    void blockWithPin();
    TopoDS_Face faceOf(const std::string& body, const gp_Dir& normal, double offset) const
    {
        for (TopExp_Explorer ex(m_window->document().findOperation(body)->result, TopAbs_FACE); ex.More(); ex.Next())
        {
            const auto d = cad::modeling::describeFace(TopoDS::Face(ex.Current()));
            if (d.normal.Angle(normal) < 1e-6 && std::fabs(d.offset - offset) < 1e-6)
                return TopoDS::Face(ex.Current());
        }
        return TopoDS_Face();
    }
    TopoDS_Face topFace(const std::string& body) const
    {
        for (TopExp_Explorer ex(m_window->document().findOperation(body)->result, TopAbs_FACE); ex.More(); ex.Next())
        {
            const auto d = cad::modeling::describeFace(TopoDS::Face(ex.Current()));
            if (d.normal.Z() > 0.999 && std::fabs(d.offset - 20.0) < 1e-6)
                return TopoDS::Face(ex.Current());
        }
        return TopoDS_Face();
    }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE571Ui::init()
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

void TestE571Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE571Ui::blockWithPin()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    m_window->useRealExtrusionDialog = false;
    m_window->nextExtrusion = ExtrudeParameters{"Bloco", "", 20.0};
    m_window->selectTreeItem("sk-1");
    action("actionExtrusaoUnificada")->trigger();
    QCOMPARE(m_window->document().operations().size(), size_t(1));
    m_window->useRealExtrusionDialog = true;
    QVERIFY(m_window->startSketchOnFace(topFace("op-1")));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{20, 30, 5}));
    QVERIFY(m_window->finishSketch());
    iso();
}

void TestE571Ui::condicoesNoDialogo()
{
    blockWithPin();
    m_window->selectTreeItem("sk-2");
    bool checked = false;
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        auto* type = d->findChild<QComboBox*>("campoTipoOperacao");
        auto* end = d->findChild<QComboBox*>("campoCondicaoFinal");
        auto* depth = d->findChild<QDoubleSpinBox*>("campoProfundidade");
        auto* outside = d->findChild<QCheckBox*>("opcaoCortarLadoDeFora");
        auto* dir2 = d->findChild<QCheckBox*>("opcaoDirecao2");
        auto* pick = d->findChild<QPushButton*>("botaoSelecionarFaceLimite");
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY(type && end && depth && outside && dir2 && pick && message);
        QCOMPARE(end->count(), 6);
        // Novo corpo: só profundidade, plano médio e até a face.
        type->setCurrentIndex(type->findData(int(ExtrudeMode::NewBody)));
        QVERIFY(enabledItem(end, EndCondition::Blind) && enabledItem(end, EndCondition::MidPlane)
                && enabledItem(end, EndCondition::UpToFace));
        QVERIFY(!enabledItem(end, EndCondition::ThroughAll) && !enabledItem(end, EndCondition::ThroughAllBoth)
                && !enabledItem(end, EndCondition::UpToNext));
        // Plano médio: profundidade total simétrica; direção 2 indisponível.
        end->setCurrentIndex(end->findData(int(EndCondition::MidPlane)));
        depth->setValue(10.0);
        QVERIFY(depth->isEnabled() && !dir2->isEnabled());
        QVERIFY2(message->text().contains(QStringLiteral("785,40")), qPrintable(message->text()));
        // Corte: todas; até a próxima desabilita "Cortar o lado de fora" e a profundidade.
        type->setCurrentIndex(type->findData(int(ExtrudeMode::Cut)));
        QVERIFY(enabledItem(end, EndCondition::UpToNext) && enabledItem(end, EndCondition::ThroughAllBoth));
        end->setCurrentIndex(end->findData(int(EndCondition::UpToNext)));
        QVERIFY(!outside->isEnabled() && !depth->isEnabled());
        QVERIFY2(message->text().contains(QStringLiteral("118.429,20")), qPrintable(message->text()));
        end->setCurrentIndex(end->findData(int(EndCondition::ThroughAllBoth)));
        QVERIFY(outside->isEnabled() && !dir2->isEnabled());
        // Até a face sem face escolhida: botão habilitado e mensagem explicando.
        end->setCurrentIndex(end->findData(int(EndCondition::UpToFace)));
        QVERIFY(pick->isEnabled());
        QVERIFY2(message->text().contains(QStringLiteral("face limite")), qPrintable(message->text()));
        saveEvidence(d->grab().toImage(), "E571_01_dialogo_condicoes");
        checked = true;
    });
    action("actionExtrusaoUnificada")->trigger();
    QVERIFY(checked);
    QCOMPARE(m_window->document().operations().size(), size_t(1));
}

void TestE571Ui::direcao2PeloDialogo()
{
    blockWithPin();
    m_window->selectTreeItem("sk-2");
    bool checked = false;
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        d->findChild<QComboBox*>("campoTipoOperacao")->setCurrentIndex(0); // novo corpo
        d->findChild<QDoubleSpinBox*>("campoProfundidade")->setValue(10.0);
        auto* dir2 = d->findChild<QCheckBox*>("opcaoDirecao2");
        auto* end2 = d->findChild<QComboBox*>("campoCondicaoFinal2");
        auto* depth2 = d->findChild<QDoubleSpinBox*>("campoProfundidade2");
        QVERIFY(!end2->isEnabled() && !depth2->isEnabled());
        dir2->setChecked(true);
        QVERIFY(end2->isEnabled() && depth2->isEnabled());
        QVERIFY(!enabledItem(end2, EndCondition::ThroughAll)); // novo corpo: sem destino
        depth2->setValue(5.0);
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY2(message->text().contains(QStringLiteral("1.178,10")), qPrintable(message->text()));
        checked = true;
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    });
    action("actionExtrusaoUnificada")->trigger();
    QVERIFY(checked);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const auto* op = m_window->document().findOperation("op-2");
    QVERIFY(op && op->direction2);
    QVERIFY(std::fabs(ShapeInspector::volume(op->result) - kPi * 25 * 15) < 1e-6);
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "operacao", "op-2")).contains(QStringLiteral("Direção 2: 5,00 mm")));
}

void TestE571Ui::ateAFaceComSelecaoReal()
{
    blockWithPin();
    // Corpo B: novo corpo sobre a face superior, topo em z = 45 (face limite).
    QVERIFY(m_window->startSketchOnFace(topFace("op-1")));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{60, 10, 30, 40}));
    QVERIFY(m_window->finishSketch());
    m_window->useRealExtrusionDialog = false;
    m_window->nextExtrusion = ExtrudeParameters{"B", "", 25.0};
    m_window->selectTreeItem("sk-3");
    action("actionExtrusaoUnificada")->trigger();
    QCOMPARE(m_window->document().operations().size(), size_t(2));
    m_window->useRealExtrusionDialog = true;
    iso();
    // Pino: saliência "Até a face"; a face limite é clicada no visualizador (laço real).
    m_window->clickLimitFace = [this] { click(viewer(), at3d(75, 30, 45)); };
    int opened = 0;
    std::function<void(ExtrusionDialog*)> second = [&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        ++opened;
        auto* label = d->findChild<QLabel*>("rotuloFaceLimite");
        QVERIFY2(label->text().contains(QStringLiteral("op-2:topo")), qPrintable(label->text()));
        QCOMPARE(d->findChild<QComboBox*>("campoCondicaoFinal")->currentData().toInt(), int(EndCondition::UpToFace));
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY2(message->text().contains(QStringLiteral("121.963,50")), qPrintable(message->text()));
        saveEvidence(d->grab().toImage(), "E571_02_ate_a_face_dialogo");
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    };
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        ++opened;
        auto* type = d->findChild<QComboBox*>("campoTipoOperacao");
        type->setCurrentIndex(type->findData(int(ExtrudeMode::Boss)));
        auto* end = d->findChild<QComboBox*>("campoCondicaoFinal");
        end->setCurrentIndex(end->findData(int(EndCondition::UpToFace)));
        // Reabertura do diálogo após a seleção da face.
        QTimer::singleShot(600, [&] { whenDialogOpens<ExtrusionDialog>(second); });
        d->findChild<QPushButton*>("botaoSelecionarFaceLimite")->click();
    });
    m_window->selectTreeItem("sk-2");
    action("actionExtrusaoUnificada")->trigger();
    QCOMPARE(opened, 2);
    QCOMPARE(m_window->limitAsked, 1);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const auto* pin = m_window->document().findOperation("op-3");
    QVERIFY(pin && pin->endCondition == EndCondition::UpToFace && pin->limitBodyId == "op-2");
    QVERIFY(std::fabs(ShapeInspector::volume(pin->result) - (120000.0 + kPi * 25 * 25)) < 1e-3);
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "operacao", "op-3"))
                .contains(QStringLiteral("Condição final: Até a face")));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E571_03_ate_a_face");
    // Alteração da geometria da face (B mais alto): extensão recalculada.
    ExtrudeParameters taller = m_window->document().extrudeParameters("op-2");
    taller.depth = 35.0;
    QVERIFY(m_window->updateExtrusion("op-2", taller));
    QVERIFY(std::fabs(ShapeInspector::volume(m_window->document().findOperation("op-3")->result)
                      - (120000.0 + kPi * 25 * 35))
            < 1e-3);
}

// Sessão real da E5.7.1: ao EDITAR uma extrusão, a face limite é clicada na peça exibida, que já
// contém o resultado da própria operação. A referência deve ir para o corpo anterior (sem
// dependência circular).
void TestE571Ui::ateAFaceNaEdicao()
{
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0}));
    // Saliência frontal: face superior em z = 15 (face limite).
    QVERIFY(m_window->startSketchOnFace(faceOf("op-1", gp_Dir(0, -1, 0), 0.0)));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{30, 5, 40, 10}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Frontal", "sk-2", 10.0, false, ExtrudeMode::Boss, "op-1"}));
    // Pino de 10 mm sobre a face superior (a operação que será editada).
    QVERIFY(m_window->startSketchOnFace(faceOf("op-2", gp_Dir(0, 0, 1), 20.0)));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{80, 30, 5}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Pino", "sk-3", 10.0, false, ExtrudeMode::Boss, "op-2"}));
    iso();
    m_window->clickLimitFace = [this] { click(viewer(), at3d(50, -5, 15)); };
    int opened = 0;
    std::function<void(ExtrusionDialog*)> second = [&](ExtrusionDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        ++opened;
        auto* label = d->findChild<QLabel*>("rotuloFaceLimite");
        QVERIFY2(label->text().contains(QStringLiteral("(op-2)")), qPrintable(label->text()));
        auto* message = d->findChild<QLabel*>("mensagemExtrusao");
        QVERIFY2(message->text().contains(QStringLiteral("123.607,30")), qPrintable(message->text()));
        d->findChild<QDialogButtonBox*>("botoesExtrusao")->button(QDialogButtonBox::Ok)->click();
    };
    whenDialogOpens<ExtrusionDialog>([&](ExtrusionDialog* d) {
        ++opened;
        auto* type = d->findChild<QComboBox*>("campoTipoOperacao");
        type->setCurrentIndex(type->findData(int(ExtrudeMode::Cut)));
        auto* end = d->findChild<QComboBox*>("campoCondicaoFinal");
        end->setCurrentIndex(end->findData(int(EndCondition::UpToFace)));
        QTimer::singleShot(600, [&] { whenDialogOpens<ExtrusionDialog>(second); });
        d->findChild<QPushButton*>("botaoSelecionarFaceLimite")->click();
    });
    m_window->selectTreeItem("op-3");
    action("actionEditarOperacao")->trigger();
    QCOMPARE(opened, 2);
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    const auto* pin = m_window->document().findOperation("op-3");
    QVERIFY(pin->extrudeMode == ExtrudeMode::Cut && pin->endCondition == EndCondition::UpToFace);
    QCOMPARE(QString::fromStdString(pin->limitBodyId), QStringLiteral("op-2"));
    QVERIFY(pin->status == OperationStatus::Valid);
    QVERIFY(std::fabs(ShapeInspector::volume(pin->result) - (124000.0 - kPi * 25 * 5)) < 1e-3);
    // Face criada pela própria operação: recusada (sem corpo anterior com a mesma face).
    TopoDS_Face bottom; // fundo do furo, z = 15, criado pela própria operação
    for (TopExp_Explorer ex(pin->result, TopAbs_FACE); ex.More(); ex.Next())
    {
        const auto d = cad::modeling::describeFace(TopoDS::Face(ex.Current()));
        if (d.normal.Z() > 0.999 && std::fabs(d.offset - 15.0) < 1e-6 && std::fabs(d.centroid.X() - 80.0) < 1e-6)
            bottom = TopoDS::Face(ex.Current());
    }
    QVERIFY(!bottom.IsNull());
    const auto own = m_window->document().faceReference("op-3", bottom);
    QVERIFY(own);
    QVERIFY(!m_window->document().limitFaceBefore("op-3", "op-3", *own));
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E571_05_ate_a_face_na_edicao");
}

void TestE571Ui::cadeiaPeloNomeTopologico()
{
    // Saliência retangular; esboço sobre a face lateral direita dela; corte; largura alterada.
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{0, 0, 100, 60}));
    QVERIFY(m_window->finishSketch());
    QVERIFY(m_window->addExtrusion({"Bloco", "sk-1", 20.0}));
    QVERIFY(m_window->startSketchOnFace(topFace("op-1")));
    QVERIFY(m_window->addSketchGeometry(SketchRectangle{40, 10, 20, 40}));
    QVERIFY(m_window->finishSketch());
    ExtrudeParameters boss{"Saliência", "sk-2", 20.0, false, ExtrudeMode::Boss, "op-1"};
    QVERIFY(m_window->addExtrusion(boss));
    TopoDS_Face side;
    for (TopExp_Explorer ex(m_window->document().findOperation("op-2")->result, TopAbs_FACE); ex.More(); ex.Next())
    {
        const auto d = cad::modeling::describeFace(TopoDS::Face(ex.Current()));
        if (d.normal.X() > 0.999 && std::fabs(d.offset - 60.0) < 1e-6)
            side = TopoDS::Face(ex.Current());
    }
    QVERIFY(!side.IsNull());
    QVERIFY(m_window->startSketchOnFace(side));
    const gp_Pnt2d c = m_window->editingSketch()->plane().toLocal(gp_Pnt(60, 30, 30));
    QVERIFY(m_window->addSketchGeometry(SketchCircle{c.X(), c.Y(), 4}));
    QVERIFY(m_window->finishSketch());
    ExtrudeParameters cut{"Corte", "sk-3", 5.0, false, ExtrudeMode::Cut, "op-2"};
    QVERIFY(m_window->addExtrusion(cut));
    // Esboço da saliência alterado pela edição: a face lateral vai para x = 90.
    QVERIFY(m_window->editSketch("sk-2"));
    QVERIFY(m_window->updateSketchEntity("e-1", SketchRectangle{40, 10, 50, 40}));
    QVERIFY(m_window->finishSketch());
    QVERIFY2(m_window->errors.isEmpty(), qPrintable(m_window->errors.join('\n')));
    QVERIFY(std::fabs(m_window->document().findSketch("sk-3")->plane().origin().X() - 90.0) < 1e-6);
    QVERIFY(m_window->document().findOperation("op-3")->status == OperationStatus::Valid);
    // Árvore: suporte da face com a normal; diagnóstico sem erros nem avisos.
    QVERIFY(childTexts(topItem(m_window->operationsTree(), "esboco", "sk-3")).first().contains(QStringLiteral("(1,00; 0,00; 0,00)")));
    QCOMPARE(m_window->diagnosticsList()->topLevelItemCount(), 0);
    QVERIFY(waitFrames(viewer()));
    saveEvidence(m_window->grab().toImage(), "E571_04_cadeia_pelo_nome");
}

QTEST_MAIN(TestE571Ui)
#include "test_e571_ui.moc"
