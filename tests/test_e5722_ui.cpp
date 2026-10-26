// E5.7.2.2 na interface real, com contexto OpenGL real (plataforma windows):
//  - diálogo da discretização: botão "Exportar ANSYS APDL" independente do "Exportar ASCII",
//    opções (unidade em metros por padrão, /CLEAR habilitado por padrão com aviso);
//  - cubo 4 × 4 × 4 mm, lco = 1 mm: exportação pelo botão, nome sugerido "<peça>_PREP7.inp",
//    189 nós e 956 elementos, comparação automática com o arquivo de referência do usuário;
//  - alterar lco desabilita as exportações até a regeneração; exportação em mm sem /CLEAR;
//  - exportação ASCII continua funcionando; confirmação de sobrescrita; cancelamento;
//  - opções lembradas ao reabrir o diálogo.
#include "ViewerTestUtils.h"

#include "app/DiscretizationDialog.h"
#include "app/MainWindow.h"
#include "discretization/ApdlExporter.h"
#include "discretization/NodeAsciiExporter.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
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

#include <filesystem>
#include <functional>
#include <optional>

#ifndef CAD_APDL_REFERENCE
#define CAD_APDL_REFERENCE ""
#endif

using cad::app::DiscretizationDialog;
using cad::app::MainWindow;
using cad::discretization::ApdlExporter;
using cad::discretization::LengthUnit;
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
    QString lastApdlSuggested;
    int apdlAsked = 0;
    bool allowOverwrite = true;
    int overwriteAsked = 0;
    QStringList errors, infos;

protected:
    QString askNodeExportPath(const QString&, LengthUnit) override { return nextNodePath; }
    QString askApdlExportPath(const QString& suggested) override
    {
        ++apdlAsked;
        lastApdlSuggested = suggested;
        return nextApdlPath;
    }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    bool confirmOverwrite(const QString&) override
    {
        ++overwriteAsked;
        return allowOverwrite;
    }
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

struct Controls
{
    QDoubleSpinBox* lco = nullptr;
    QComboBox* asciiUnit = nullptr;
    QComboBox* apdlUnit = nullptr;
    QCheckBox* apdlClear = nullptr;
    QLabel* clearWarning = nullptr;
    QPushButton* generate = nullptr;
    QPushButton* exportAscii = nullptr;
    QPushButton* exportApdl = nullptr;
    QPushButton* close = nullptr;

    explicit Controls(DiscretizationDialog* d)
    {
        lco = d->findChild<QDoubleSpinBox*>("campoLco");
        asciiUnit = d->findChild<QComboBox*>("campoUnidade");
        apdlUnit = d->findChild<QComboBox*>("campoUnidadeApdl");
        apdlClear = d->findChild<QCheckBox*>("opcaoApdlClear");
        clearWarning = d->findChild<QLabel*>("avisoApdlClear");
        generate = d->findChild<QPushButton*>("botaoGerar");
        exportAscii = d->findChild<QPushButton*>("botaoExportar");
        exportApdl = d->findChild<QPushButton*>("botaoExportarApdl");
        close = d->findChild<QPushButton*>("botaoFechar");
    }
    bool complete() const
    {
        return lco && asciiUnit && apdlUnit && apdlClear && clearWarning && generate && exportAscii && exportApdl
               && close;
    }
};

} // namespace

class TestE5722Ui : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void exportacaoApdlPeloDialogo();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    OcctViewerWidget* viewer() const { return m_window->viewer(); }
    void boxBody(double x, double y, double w, double h, double depth, const char* name)
    {
        QVERIFY(m_window->startNewSketch(PlaneType::XY));
        QVERIFY(m_window->addSketchGeometry(SketchRectangle{x, y, w, h}));
        QVERIFY(m_window->finishSketch());
        const std::string sketchId = m_window->document().sketches().back().id();
        QVERIFY(m_window->addExtrusion(ExtrudeParameters{name, sketchId, depth}));
    }
    QString path(const QString& name) const { return m_dir->filePath(name); }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestE5722Ui::init()
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

void TestE5722Ui::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestE5722Ui::exportacaoApdlPeloDialogo()
{
    boxBody(0, 0, 4, 4, 4, "Cubo 4x4x4");
    action("actionVistaIsometrica")->trigger();
    QVERIFY(waitFrames(viewer()));
    const TopoDS_Shape original = m_window->document().shape();
    const int revision = m_window->document().revision();
    const QString apdlM = path("cubo_PREP7.inp");
    const QString apdlMm = path("cubo_lco2_mm_PREP7.inp");
    const QString ascii = path("cubo_nodes.txt");
    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        Controls c(d);
        QVERIFY(c.complete());
        // Padrões: metros e /CLEAR habilitado (reproduzem o exemplo), com aviso visível.
        QCOMPARE(LengthUnit(c.apdlUnit->currentData().toInt()), LengthUnit::Meter);
        QVERIFY(c.apdlClear->isChecked());
        QVERIFY(c.clearWarning->text().contains("/CLEAR"));
        QVERIFY(c.clearWarning->text().contains("apaga o banco de dados"));
        QCOMPARE(c.exportApdl->text(), QString("Exportar ANSYS APDL..."));
        QCOMPARE(c.exportAscii->text(), QString("Exportar ASCII..."));
        QVERIFY(!c.exportApdl->isEnabled());
        QVERIFY(!c.exportAscii->isEnabled());

        c.lco->setValue(1.0);
        c.generate->click();
        QVERIFY(m_window->discretization());
        QCOMPARE(m_window->discretization()->totalNodeCount(), std::int64_t(189));
        QVERIFY(c.exportApdl->isEnabled());
        QVERIFY(c.exportAscii->isEnabled());
        QVERIFY(waitFrames(viewer(), 2));
        saveEvidence(d->grab().toImage(), "E5722_01_dialogo_exportacao_apdl");

        // Cancelamento do seletor de arquivo: nada é gravado.
        m_window->nextApdlPath.clear();
        c.exportApdl->click();
        QCOMPARE(m_window->apdlAsked, 1);
        QVERIFY(m_window->lastApdlSuggested.endsWith("_PREP7.inp"));

        // Exportação em metros com /CLEAR (caso de referência); extensão .inp acrescentada.
        m_window->infos.clear();
        m_window->nextApdlPath = apdlM.chopped(4);
        c.exportApdl->click();
        QVERIFY2(QFileInfo::exists(apdlM), qPrintable(m_window->errors.join('\n')));
        QVERIFY(m_window->errors.isEmpty());
        QCOMPARE(m_window->infos.size(), 1);
        QVERIFY(m_window->infos.front().contains("956"));
        QVERIFY(m_window->infos.front().contains("/CLEAR"));
        const QStringList lines = readLines(apdlM);
        QCOMPARE(lines.value(0), QString("!seed == 10"));
        QCOMPARE(lines.value(2), QString("/CLEAR"));
        QVERIFY(lines.contains("N,126,0.0005,0.0005,0.0005"));
        QVERIFY(lines.contains("N,189,0.0035,0.0035,0.0035"));
        QCOMPARE(lines.filter(QRegularExpression("^N,")).size(), 189);
        QCOMPARE(lines.filter(QRegularExpression("^EN,")).size(), 956);
        QCOMPARE(lines.last(), QString("EN,956,189,125"));
        const std::filesystem::path reference = std::filesystem::u8path(CAD_APDL_REFERENCE);
        if (std::filesystem::exists(reference))
        {
            const auto cmp = ApdlExporter::compare(apdlM.toStdWString(), reference);
            QVERIFY2(cmp.ok, cmp.summary.c_str());
        }
        else
            qWarning("Arquivo de referência ausente: comparação automática não executada");

        // Sobrescrita recusada: arquivo existente mantido.
        QFile original(apdlM);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray before = original.readAll();
        original.close();
        m_window->allowOverwrite = false;
        m_window->nextApdlPath = apdlM;
        c.exportApdl->click();
        QCOMPARE(m_window->overwriteAsked, 1);
        QFile again(apdlM);
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), before);
        m_window->allowOverwrite = true;

        // A exportação ASCII continua independente e funcionando.
        m_window->nextNodePath = ascii;
        c.exportAscii->click();
        QVERIFY(QFileInfo::exists(ascii));
        QCOMPARE(readLines(ascii).value(0), QString("ID_node Coord_X Coord_Y Coord_Z"));
        QCOMPARE(readLines(ascii).size(), 190);

        // Alterar lco exige regenerar antes de exportar.
        c.lco->setValue(2.0);
        QVERIFY(!c.exportApdl->isEnabled());
        QVERIFY(!c.exportAscii->isEnabled());
        QVERIFY(c.exportApdl->toolTip().contains("Gere a discretização"));
        c.generate->click();
        QVERIFY(c.exportApdl->isEnabled());
        QCOMPARE(m_window->discretization()->cubeCount(), std::int64_t(8));

        // Milímetros sem /CLEAR.
        c.apdlUnit->setCurrentIndex(c.apdlUnit->findData(int(LengthUnit::Millimeter)));
        c.apdlClear->setChecked(false);
        m_window->infos.clear();
        m_window->nextApdlPath = apdlMm;
        c.exportApdl->click();
        QVERIFY(QFileInfo::exists(apdlMm));
        QVERIFY(m_window->infos.size() == 1 && m_window->infos.front().contains("/CLEAR omitido"));
        const QStringList mm = readLines(apdlMm);
        QVERIFY(!mm.contains("/CLEAR"));
        QVERIFY(mm.contains("N,2,2,0,0"));
        QVERIFY(mm.contains("! Unidade das coordenadas: milimetros (mm). O APDL nao impoe um sistema de unidades: os dados"));
        QCOMPARE(mm.filter(QRegularExpression("^EN,")).size(), 54 + 12 + 64);
        saveEvidence(d->grab().toImage(), "E5722_02_dialogo_mm_sem_clear");
        c.close->click();
        reached = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reached);

    // Opções lembradas ao reabrir o diálogo.
    bool reopened = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        Controls c(d);
        QVERIFY(c.complete());
        QCOMPARE(LengthUnit(c.apdlUnit->currentData().toInt()), LengthUnit::Millimeter);
        QVERIFY(!c.apdlClear->isChecked());
        QVERIFY(c.exportApdl->isEnabled()); // a discretização (lco = 2) continua válida
        c.close->click();
        reopened = true;
    });
    action("actionDiscretizacao")->trigger();
    QVERIFY(reopened);

    // O sólido e o documento não foram alterados pelas exportações.
    QVERIFY(m_window->document().shape().IsSame(original));
    QCOMPARE(m_window->document().revision(), revision);
}

QTEST_MAIN(TestE5722Ui)
#include "test_e5722_ui.moc"
