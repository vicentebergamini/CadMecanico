// E5.7.2 na interface real, com contexto OpenGL real (plataforma windows):
//  - Ferramentas > Discretização volumétrica: diálogo, seleção do sólido, lco, critério, geração
//    (thread de trabalho com progresso), resultados, exibição na janela 3D (sólido, nós dos
//    vértices, nós centrais, contornos), alteração de lco e exportação ASCII pelo botão;
//  - bloco de referência 100 × 100 × 10 mm com lco = 1 mm (100.000 cubos, 212.211 nós) e tempos;
//  - cancelamento da geração sem alterar o documento;
//  - vários sólidos: seleção explícita obrigatória;
//  - Arquivo > Exportar > Nuvem de pontos ASCII (unidade, confirmação de sobrescrita);
//  - descarte da discretização quando o sólido de origem muda;
//  - amostra real exportada (sequência de referência do usuário) para o relatório.
#include "ViewerTestUtils.h"

#include "app/DiscretizationDialog.h"
#include "app/MainWindow.h"
#include "discretization/NodeAsciiExporter.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>

#include <functional>
#include <optional>

using cad::app::DiscretizationDialog;
using cad::app::MainWindow;
using cad::discretization::LengthUnit;
using cad::discretization::NodeAsciiExporter;
using cad::document::BooleanParameters;
using cad::document::ExtrudeParameters;
using cad::modeling::BooleanKind;
using cad::viewer::OcctViewerWidget;
using namespace cad::sketch;
using namespace cad::test::viewer;

namespace {

class TestableMainWindow : public MainWindow
{
public:
    QString nextExportPath;
    std::optional<LengthUnit> nextUnit;
    QList<LengthUnit> exportUnits;
    bool allowOverwrite = true;
    int overwriteAsked = 0;
    QStringList errors, infos;

protected:
    QString askNodeExportPath(const QString& suggested, LengthUnit unit) override
    {
        exportUnits << unit;
        lastSuggested = suggested;
        return nextExportPath;
    }
    std::optional<LengthUnit> askNodeExportUnit(LengthUnit) override { return nextUnit; }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    bool confirmOverwrite(const QString&) override
    {
        ++overwriteAsked;
        return allowOverwrite;
    }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }

public:
    QString lastSuggested;
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

QStringList readLines(const QString& path, int max = -1)
{
    QFile f(path);
    QStringList lines;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return lines;
    QTextStream in(&f);
    while (!in.atEnd() && (max < 0 || lines.size() < max))
        lines << in.readLine();
    return lines;
}

int countLines(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    int n = 0;
    while (!f.atEnd())
    {
        f.readLine();
        ++n;
    }
    return n;
}

// Controles do diálogo.
struct Controls
{
    QComboBox* body = nullptr;
    QDoubleSpinBox* lco = nullptr;
    QComboBox* criterion = nullptr;
    QComboBox* unit = nullptr;
    QLabel* estimate = nullptr;
    QProgressBar* progress = nullptr;
    QLabel* result = nullptr;
    QLabel* message = nullptr;
    QCheckBox* showSolid = nullptr;
    QCheckBox* showVertices = nullptr;
    QCheckBox* showCenters = nullptr;
    QCheckBox* showEdges = nullptr;
    QPushButton* generate = nullptr;
    QPushButton* exportButton = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* close = nullptr;

    explicit Controls(DiscretizationDialog* d)
    {
        body = d->findChild<QComboBox*>("campoSolido");
        lco = d->findChild<QDoubleSpinBox*>("campoLco");
        criterion = d->findChild<QComboBox*>("campoCriterio");
        unit = d->findChild<QComboBox*>("campoUnidade");
        estimate = d->findChild<QLabel*>("rotuloEstimativa");
        progress = d->findChild<QProgressBar*>("barraProgresso");
        result = d->findChild<QLabel*>("resultadoDiscretizacao");
        message = d->findChild<QLabel*>("mensagemDiscretizacao");
        showSolid = d->findChild<QCheckBox*>("opcaoExibirSolido");
        showVertices = d->findChild<QCheckBox*>("opcaoExibirNosVertices");
        showCenters = d->findChild<QCheckBox*>("opcaoExibirNosCentrais");
        showEdges = d->findChild<QCheckBox*>("opcaoExibirContornos");
        generate = d->findChild<QPushButton*>("botaoGerar");
        exportButton = d->findChild<QPushButton*>("botaoExportar");
        cancel = d->findChild<QPushButton*>("botaoCancelar");
        close = d->findChild<QPushButton*>("botaoFechar");
    }
    bool complete() const
    {
        return body && lco && criterion && unit && estimate && progress && result && message && showSolid
               && showVertices && showCenters && showEdges && generate && exportButton && cancel && close;
    }
};

} // namespace

class TestE572Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void blocoReferenciaPeloDialogo();
    void cancelamentoDaGeracao();
    void variosSolidosSelecaoExplicita();
    void exportacaoPeloMenu();
    void descarteQuandoOSolidoMuda();
    void amostraSequenciaDeReferencia();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    void iso()
    {
        action("actionVistaIsometrica")->trigger();
        QVERIFY(waitFrames(viewer()));
    }
    // Corpo prismático a partir de um retângulo no plano XY (z = 0 .. depth).
    void boxBody(double x, double y, double w, double h, double depth, const char* name)
    {
        QVERIFY(m_window->startNewSketch(PlaneType::XY));
        QVERIFY(m_window->addSketchGeometry(SketchRectangle{x, y, w, h}));
        QVERIFY(m_window->finishSketch());
        const std::string sketchId = m_window->document().sketches().back().id();
        QVERIFY(m_window->addExtrusion(ExtrudeParameters{name, sketchId, depth}));
    }
    bool isDisplayed(const occ::handle<AIS_InteractiveObject>& o) const
    {
        return !o.IsNull() && viewer()->aisContext()->IsDisplayed(o);
    }
    QString path(const QString& name) const { return m_dir->filePath(name); }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE572Ui::init()
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

void TestE572Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE572Ui::blocoReferenciaPeloDialogo()
{
    boxBody(0, 0, 100, 100, 10, "Bloco 100x100x10");
    iso();
    QVERIFY(action("actionDiscretizacao")->isEnabled());
    QVERIFY(!action("actionExportarNuvemPontos")->isEnabled());
    const QImage before = viewer()->grabViewerImage();
    const TopoDS_Shape original = m_window->document().shape();
    const int revision = m_window->document().revision();
    const QString exported = path("bloco_nodes_m.txt");
    m_window->nextExportPath = exported;
    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        Controls c(d);
        QVERIFY(c.complete());
        QCOMPARE(m_window->discretizationDialog(), d);
        // Um único sólido: já selecionado; critério 1 por padrão; lco em mm.
        QCOMPARE(c.body->count(), 1);
        QCOMPARE(c.body->currentData().toString(), QString("op-1"));
        QCOMPARE(c.criterion->currentIndex(), 0);
        QVERIFY(c.lco->suffix().contains("mm"));
        c.lco->setValue(1.0);
        QVERIFY(c.estimate->text().contains("100 × 100 × 10"));
        QVERIFY(!c.exportButton->isEnabled());
        saveEvidence(d->grab().toImage(), "E572_01_dialogo_antes");

        QElapsedTimer t;
        t.start();
        c.generate->click(); // geração na thread de trabalho; a interface processa eventos
        const qint64 ms = t.elapsed();
        QVERIFY(!m_window->isDiscretizing());
        const auto disc = m_window->discretization();
        QVERIFY(disc);
        QCOMPARE(disc->cubeCount(), std::int64_t(100000));
        QCOMPARE(disc->vertexNodeCount(), std::int64_t(112211));
        QCOMPARE(disc->centerNodeCount(), std::int64_t(100000));
        QCOMPARE(disc->totalNodeCount(), std::int64_t(212211));
        QCOMPARE(c.progress->value(), 1000);
        const QString text = c.result->text();
        for (const char* expected : {"100.000", "112.211", "212.211", "100.000,000 mm³", "Cubo totalmente contido"})
            QVERIFY2(text.contains(QString::fromUtf8(expected)), expected);
        QVERIFY(c.exportButton->isEnabled());
        qInfo("E572 desempenho UI: geração+exibição %lld ms; cálculo %.3f s; visualização %.3f s; overlay %lld+%lld "
              "pontos, %lld segmentos",
              ms, disc->stats().totalSeconds, m_window->discretizationDisplaySeconds(),
              (long long)viewer()->discretizationOverlayStats().vertexPoints,
              (long long)viewer()->discretizationOverlayStats().centerPoints,
              (long long)viewer()->discretizationOverlayStats().segments);
        QCOMPARE(viewer()->discretizationOverlayStats().segments, std::int64_t(2 * 100 * 101 * 11 + 101 * 101 * 10));

        // Visualização: objetos agrupados exibidos; sólido translúcido sobreposto.
        QVERIFY(isDisplayed(viewer()->vertexNodesPresentation()));
        QVERIFY(isDisplayed(viewer()->centerNodesPresentation()));
        QVERIFY(isDisplayed(viewer()->cubeEdgesPresentation()));
        QVERIFY(viewer()->isSolidDisplayed());
        QVERIFY(waitFrames(viewer(), 2));
        const QImage overlay = viewer()->grabViewerImage();
        saveEvidence(overlay, "E572_02_bloco_discretizado_sobreposto");
        saveEvidence(d->grab().toImage(), "E572_03_dialogo_resultado");
        QVERIFY(diff(before, overlay).count > 1000);
        // Ocultar o sólido; exibir somente os nós.
        c.showSolid->setChecked(false);
        QVERIFY(!viewer()->isSolidDisplayed());
        QVERIFY(!action("actionExibirSolido")->isChecked());
        c.showEdges->setChecked(false);
        QVERIFY(!isDisplayed(viewer()->cubeEdgesPresentation()));
        QVERIFY(waitFrames(viewer(), 2));
        const QImage nodesOnly = viewer()->grabViewerImage();
        saveEvidence(nodesOnly, "E572_04_somente_nos");
        // Os vértices (azuis) cobrem os centros na vista isométrica do bloco denso: ocultá-los
        // revela os nós centrais (vermelhos); ocultar também os centros deixa a vista vazia.
        c.showVertices->setChecked(false);
        QVERIFY(!isDisplayed(viewer()->vertexNodesPresentation()));
        QVERIFY(waitFrames(viewer(), 2));
        const QImage centersOnly = viewer()->grabViewerImage();
        saveEvidence(centersOnly, "E572_04b_somente_nos_centrais");
        QVERIFY(diff(nodesOnly, centersOnly).count > 1000);
        c.showCenters->setChecked(false);
        QVERIFY(!isDisplayed(viewer()->centerNodesPresentation()));
        QVERIFY(waitFrames(viewer(), 2));
        QVERIFY(diff(centersOnly, viewer()->grabViewerImage()).count > 1000);
        c.showEdges->setChecked(true);
        c.showVertices->setChecked(false);
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(viewer()->grabViewerImage(), "E572_05_contornos_dos_cubos");
        // Restaura tudo.
        for (QCheckBox* box : {c.showSolid, c.showVertices, c.showCenters, c.showEdges})
            box->setChecked(true);
        QVERIFY(viewer()->isSolidDisplayed() && isDisplayed(viewer()->vertexNodesPresentation()));

        // Exportação ASCII em metros pelo botão.
        c.unit->setCurrentIndex(c.unit->findData(int(LengthUnit::Meter)));
        t.restart();
        c.exportButton->click();
        qInfo("E572 desempenho UI: exportação %lld ms", t.elapsed());
        QCOMPARE(m_window->exportUnits.size(), 1);
        QCOMPARE(m_window->exportUnits.front(), LengthUnit::Meter);
        QVERIFY(m_window->lastSuggested.endsWith("_nodes.txt"));
        QVERIFY(QFile::exists(exported));
        QVERIFY(m_window->infos.size() == 1 && m_window->infos.front().contains("metros (m)"));

        // Alteração de lco: o resultado anterior fica desatualizado até nova geração.
        c.lco->setValue(2.0);
        QVERIFY(!c.exportButton->isEnabled());
        c.generate->click();
        const auto disc2 = m_window->discretization();
        QVERIFY(disc2 && disc2 != disc);
        QCOMPARE(disc2->cubeCount(), std::int64_t(12500));
        QCOMPARE(disc2->vertexNodeCount(), std::int64_t(51 * 51 * 6));
        QVERIFY(c.result->text().contains("12.500"));
        QVERIFY(c.exportButton->isEnabled());
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(viewer()->grabViewerImage(), "E572_06_lco_2mm");
        // Volta para lco = 1 (mesmo resultado da primeira geração: reprodutível).
        c.lco->setValue(1.0);
        c.generate->click();
        QVERIFY(m_window->discretization()->cubeCells() == disc->cubeCells());
        QVERIFY(m_window->discretization()->vertexPoints() == disc->vertexPoints());
        c.close->click();
        reached = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reached);
    QVERIFY(!m_window->discretizationDialog());
    // A geometria CAD não foi alterada e o documento não foi modificado pela discretização.
    QVERIFY(m_window->document().shape().IsSame(original));
    QCOMPARE(m_window->document().revision(), revision);
    QVERIFY(action("actionExportarNuvemPontos")->isEnabled());
    // Arquivo exportado: conferido contra a discretização de lco = 1 (gerada antes da troca de lco).
    const QStringList head = readLines(exported, 4);
    QCOMPARE(head.value(0), QString("ID_node Coord_X Coord_Y Coord_Z"));
    QCOMPARE(head.value(1), QString("1 0 0 0"));
    QCOMPARE(head.value(2), QString("2 0.001 0 0"));
    QCOMPARE(countLines(exported), 212212);
    QVERIFY(NodeAsciiExporter::verify(exported.toStdWString(), *m_window->discretization(), LengthUnit::Meter).ok);
}

void TestE572Ui::cancelamentoDaGeracao()
{
    m_window->newExampleDocument(); // bloco 100 × 60 × 20 com furo passante R10
    iso();
    const TopoDS_Shape original = m_window->document().shape();
    const int revision = m_window->document().revision();
    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        Controls c(d);
        QVERIFY(c.complete());
        c.lco->setValue(0.25); // 400 × 240 × 80 candidatos: geração demorada
        bool sawRunning = false;
        QTimer::singleShot(600, [&] {
            sawRunning = m_window->isDiscretizing() && !c.generate->isEnabled() && !c.lco->isEnabled()
                         && c.cancel->isEnabled() && !action("actionNovo")->isEnabled();
            saveEvidence(d->grab().toImage(), "E572_07_geracao_em_andamento");
            c.cancel->click();
        });
        QElapsedTimer t;
        t.start();
        c.generate->click();
        qInfo("E572 cancelamento: geração interrompida após %lld ms", t.elapsed());
        QVERIFY(sawRunning);
        QVERIFY(!m_window->isDiscretizing());
        QVERIFY(!m_window->discretization());
        QVERIFY(c.message->text().contains("cancelada"));
        QVERIFY(c.generate->isEnabled() && c.lco->isEnabled());
        QVERIFY(action("actionNovo")->isEnabled());
        // Nova geração após o cancelamento.
        c.lco->setValue(2.0);
        c.generate->click();
        QVERIFY(m_window->discretization());
        QCOMPARE(m_window->discretization()->cubeCount(), std::int64_t(1412 * 10)); // furo R10, lco 2 (88 colunas removidas)
        c.close->click();
        reached = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reached);
    QVERIFY(m_window->document().shape().IsSame(original));
    QCOMPARE(m_window->document().revision(), revision);
    QVERIFY(!m_window->document().isModified());
}

void TestE572Ui::variosSolidosSelecaoExplicita()
{
    boxBody(0, 0, 10, 10, 10, "A");
    boxBody(20, 0, 10, 5, 4, "B");
    iso();
    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        Controls c(d);
        QVERIFY(c.complete());
        QCOMPARE(c.body->count(), 3); // "selecione" + dois sólidos
        QVERIFY(c.body->currentData().toString().isEmpty());
        c.generate->click();
        QVERIFY(c.message->text().contains("Selecione o sólido"));
        QVERIFY(!m_window->discretization());
        c.body->setCurrentIndex(c.body->findData(QString("op-2")));
        c.lco->setValue(1.0);
        c.generate->click();
        QVERIFY(m_window->discretization());
        QCOMPARE(m_window->discretizationBody(), QString("op-2"));
        QCOMPARE(m_window->discretization()->cubeCount(), std::int64_t(10 * 5 * 4));
        // Coordenadas reais (sólido começa em x = 20).
        QCOMPARE(m_window->discretization()->node(1).position.X(), 20.0);
        c.close->click();
        reached = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reached);
}

void TestE572Ui::exportacaoPeloMenu()
{
    boxBody(20, 0, 10, 2, 1, "Peça deslocada");
    QVERIFY(m_window->discretize("op-1", {1.0}));
    QVERIFY(action("actionExportarNuvemPontos")->isEnabled());
    const QString file = path("menu_nodes.txt");
    // Unidade cancelada: nada é gravado.
    m_window->nextUnit.reset();
    m_window->nextExportPath = file;
    action("actionExportarNuvemPontos")->trigger();
    QVERIFY(!QFile::exists(file));
    // Milímetros.
    m_window->nextUnit = LengthUnit::Millimeter;
    action("actionExportarNuvemPontos")->trigger();
    QVERIFY(QFile::exists(file));
    QCOMPARE(readLines(file, 2).value(1), QString("1 20 0 0"));
    // Arquivo existente: sem confirmação o arquivo é mantido.
    m_window->nextUnit = LengthUnit::Meter;
    m_window->allowOverwrite = false;
    action("actionExportarNuvemPontos")->trigger();
    QCOMPARE(m_window->overwriteAsked, 1);
    QCOMPARE(readLines(file, 2).value(1), QString("1 20 0 0"));
    m_window->allowOverwrite = true;
    action("actionExportarNuvemPontos")->trigger();
    QCOMPARE(readLines(file, 2).value(1), QString("1 0.02 0 0"));
    QVERIFY(NodeAsciiExporter::verify(file.toStdWString(), *m_window->discretization(), LengthUnit::Meter).ok);
    QVERIFY(m_window->errors.isEmpty());
}

void TestE572Ui::descarteQuandoOSolidoMuda()
{
    m_window->newExampleDocument();
    QVERIFY(m_window->discretize("op-2", {2.0}));
    QVERIFY(viewer()->hasDiscretization());
    QVERIFY(action("actionExportarNuvemPontos")->isEnabled());
    // Entrar e sair do esboço não descarta (o sólido é o mesmo).
    QVERIFY(m_window->startNewSketch(PlaneType::XY));
    QVERIFY(!isDisplayed(viewer()->vertexNodesPresentation()));
    m_window->cancelSketch();
    QVERIFY(isDisplayed(viewer()->vertexNodesPresentation()));
    // Alterar o furo recalcula o sólido: a discretização antiga é descartada.
    auto p = m_window->document().parameters();
    p.holeRadius = 15.0;
    QVERIFY(m_window->applyParameters(p));
    QVERIFY(!m_window->discretization());
    QVERIFY(!viewer()->hasDiscretization());
    QVERIFY(!action("actionExportarNuvemPontos")->isEnabled());
    QVERIFY(viewer()->isSolidDisplayed());
    // Remover a discretização da vista.
    QVERIFY(m_window->discretize("op-2", {2.0}));
    action("actionRemoverDiscretizacao")->trigger();
    QVERIFY(!m_window->discretization() && !viewer()->hasDiscretization());
}

void TestE572Ui::amostraSequenciaDeReferencia()
{
    // Linha y ∈ [0, 1]: x de 20 a 30 mm; linha y ∈ [1, 2]: x de 16 a 33 mm (união booleana).
    boxBody(20, 0, 10, 1, 1, "Linha 1");
    boxBody(16, 1, 17, 1, 1, "Linha 2");
    QVERIFY(m_window->addBoolean(BooleanParameters{"Peça irregular", BooleanKind::Fuse, "op-1", "op-2"}));
    iso();
    const QString sampleDir = qEnvironmentVariable("CADMECANICO_E572_SAMPLE_DIR");
    const QString file = sampleDir.isEmpty() ? path("exemplo_nodes.txt") : QDir(sampleDir).filePath("exemplo_nodes.txt");
    if (!sampleDir.isEmpty())
        QDir().mkpath(sampleDir);
    m_window->nextExportPath = file;
    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        Controls c(d);
        QVERIFY(c.complete());
        QCOMPARE(c.body->currentData().toString(), QString("op-3"));
        c.lco->setValue(1.0);
        c.generate->click();
        QVERIFY(m_window->discretization());
        QCOMPARE(m_window->discretization()->cubeCount(), std::int64_t(27));
        c.unit->setCurrentIndex(c.unit->findData(int(LengthUnit::Meter)));
        c.exportButton->click();
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(viewer()->grabViewerImage(), "E572_08_geometria_irregular");
        // Somente os nós centrais (sólido translúcido e depois oculto): visíveis nos dois casos.
        c.showVertices->setChecked(false);
        c.showEdges->setChecked(false);
        QVERIFY(waitFrames(viewer(), 2));
        const QImage centersWithSolid = viewer()->grabViewerImage();
        saveEvidence(centersWithSolid, "E572_09_centros_com_solido");
        c.showCenters->setChecked(false);
        QVERIFY(waitFrames(viewer(), 2));
        const QImage solidOnly = viewer()->grabViewerImage();
        QVERIFY(diff(centersWithSolid, solidOnly).count > 50);
        c.showCenters->setChecked(true);
        c.showSolid->setChecked(false);
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(viewer()->grabViewerImage(), "E572_10_centros_sem_solido");
        for (QCheckBox* box : {c.showSolid, c.showVertices, c.showCenters, c.showEdges})
            box->setChecked(true);
        c.close->click();
        reached = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reached);
    // Sequência fornecida pelo usuário (sem o prefixo "n" do APDL).
    const QStringList expected{
        "1 0.02 0 0",       "2 0.021 0 0",      "3 0.022 0 0",      "4 0.023 0 0",      "5 0.024 0 0",
        "6 0.025 0 0",      "7 0.026 0 0",      "8 0.027 0 0",      "9 0.028 0 0",      "10 0.029 0 0",
        "11 0.03 0 0",      "12 0.016 0.001 0", "13 0.017 0.001 0", "14 0.018 0.001 0", "15 0.019 0.001 0",
        "16 0.02 0.001 0",  "17 0.021 0.001 0", "18 0.022 0.001 0", "19 0.023 0.001 0", "20 0.024 0.001 0",
        "21 0.025 0.001 0", "22 0.026 0.001 0", "23 0.027 0.001 0", "24 0.028 0.001 0", "25 0.029 0.001 0",
        "26 0.03 0.001 0",  "27 0.031 0.001 0", "28 0.032 0.001 0", "29 0.033 0.001 0"};
    const QStringList lines = readLines(file);
    QCOMPARE(lines.value(0), QString("ID_node Coord_X Coord_Y Coord_Z"));
    QCOMPARE(lines.size(), 1 + 94 + 27);
    for (int n = 0; n < expected.size(); ++n)
        QCOMPARE(lines.value(n + 1), expected[n]);
    QVERIFY(NodeAsciiExporter::verify(file.toStdWString(), *m_window->discretization(), LengthUnit::Meter).ok);
    qInfo("E572 amostra exportada: %s", qPrintable(QDir::toNativeSeparators(file)));
}

QTEST_MAIN(TestE572Ui)
#include "test_e572_ui.moc"
