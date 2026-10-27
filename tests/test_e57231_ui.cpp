// E5.7.2.3.1 — dimensionamento da janela "Discretização volumétrica" (somente interface):
//  - geometria inicial para as áreas úteis de 1366 × 768, 1600 × 900, 1920 × 1080 e 2560 × 1440 com
//    escalas de 100 %, 125 % e 150 % (área útil lógica = resolução / escala, menos a barra de
//    tarefas): janela inteira (com moldura) dentro da área útil, altura ≤ ~90 % e centralizada;
//  - layout nesses tamanhos: botões inferiores visíveis, inteiros e fora de áreas de rolagem;
//    campos de configuração visíveis e fora de rolagem; nenhum controle sobreposto; textos sem
//    corte; rolagem vertical dos resultados com muitas linhas sem aumentar a janela;
//    redimensionamento manual;
//  - fluxo real pela janela principal no monitor atual (a escala de exibição é variada pelo CTest
//    com QT_SCALE_FACTOR): abertura dentro da área útil, geração com projeção dos nós (resultado com
//    as linhas adicionais), layout preservado e exportações ASCII e ANSYS APDL funcionando.
#include "ViewerTestUtils.h"

#include "app/DiscretizationDialog.h"
#include "app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <cmath>
#include <functional>

using cad::app::DiscretizationDialog;
using cad::app::MainWindow;
using cad::discretization::InclusionCriterion;
using cad::discretization::LengthUnit;
using cad::discretization::Parameters;
using cad::document::ExtrudeParameters;
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

int countLines(const QString& path, const QString& prefix)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    QTextStream in(&f);
    int n = 0;
    while (!in.atEnd())
        if (in.readLine().startsWith(prefix))
            ++n;
    return n;
}

// Resultado longo, no formato do resumo da janela principal (projeção com todas as linhas).
QString longResult()
{
    QString html = "<table>";
    for (int i = 0; i < 80; ++i)
        html += QString("<tr><td>Indicador de qualidade %1:</td><td>%2 mm</td></tr>").arg(i + 1).arg(0.5 + i);
    return html + "</table>";
}

struct Screen
{
    int width, height, percent;
    // Área útil lógica: a barra de tarefas tem 40 px a 100 % e acompanha a escala.
    QRect available() const
    {
        const double s = percent / 100.0;
        return QRect(0, 0, int(std::floor(width / s)), int(std::floor((height - 40.0 * s) / s)));
    }
    QString name() const { return QString("%1x%2_%3").arg(width).arg(height).arg(percent); }
};

const Screen kScreens[] = {{1366, 768, 100}, {1366, 768, 125}, {1366, 768, 150}, {1600, 900, 100},
                           {1600, 900, 125}, {1600, 900, 150}, {1920, 1080, 100}, {1920, 1080, 125},
                           {1920, 1080, 150}, {2560, 1440, 100}, {2560, 1440, 125}, {2560, 1440, 150}};

bool insideScrollArea(const QWidget* w)
{
    for (const QWidget* p = w->parentWidget(); p; p = p->parentWidget())
        if (qobject_cast<const QScrollArea*>(p))
            return true;
    return false;
}

QRect rectIn(const QWidget* w, const QWidget* root)
{
    return QRect(w->mapTo(root, QPoint(0, 0)), w->size());
}

} // namespace

class TestE57231Ui : public QObject
{
    Q_OBJECT

private slots:
    void geometriaInicialPorResolucao();
    void layoutPorResolucao();
    void fluxoRealComProjecao();

private:
    // Verifica o layout do diálogo no tamanho atual; 'tag' identifica o caso nas mensagens.
    void verifyLayout(DiscretizationDialog* d, const QString& tag);
};

void TestE57231Ui::verifyLayout(DiscretizationDialog* d, const QString& tag)
{
    const QByteArray tagBytes = tag.toLocal8Bit();
    const char* t = tagBytes.constData();
    const QRect area = d->rect();
    // Botões inferiores: visíveis, inteiros dentro da janela, texto sem corte, fora de rolagem.
    for (const char* name : {"botaoGerar", "botaoExportar", "botaoExportarApdl", "botaoCancelar", "botaoFechar"})
    {
        auto* b = d->findChild<QPushButton*>(name);
        QVERIFY2(b && b->isVisible(), t);
        QVERIFY2(area.contains(rectIn(b, d)), qPrintable(tag + " " + name));
        QVERIFY2(b->width() >= b->sizeHint().width() && b->height() >= b->minimumSizeHint().height(),
                 qPrintable(tag + " " + name));
        QVERIFY2(!insideScrollArea(b), t);
    }
    // Região superior: configurações visíveis, inteiras, sem corte e fora de rolagem.
    for (const char* name : {"campoSolido", "campoLco", "campoCriterio", "campoUnidade"})
    {
        auto* w = d->findChild<QWidget*>(name);
        QVERIFY2(w && w->isVisible(), t);
        QVERIFY2(area.contains(rectIn(w, d)), qPrintable(tag + " " + name));
        QVERIFY2(w->width() >= w->minimumSizeHint().width() && w->height() >= w->minimumSizeHint().height(),
                 qPrintable(tag + " " + name));
        QVERIFY2(!insideScrollArea(w), t);
    }
    // Nenhuma sobreposição entre os controles do layout principal (as duas áreas de rolagem ficam
    // dentro do divisor).
    QList<QWidget*> items;
    for (const char* name : {"campoSolido", "campoLco", "campoCriterio", "campoUnidade", "rotuloEstimativa",
                             "barraProgresso", "divisorResultados", "mensagemDiscretizacao", "botaoGerar",
                             "botaoExportar", "botaoExportarApdl", "botaoCancelar", "botaoFechar"})
        if (auto* w = d->findChild<QWidget*>(name); w && w->isVisible())
            items << w;
    for (int a = 0; a < items.size(); ++a)
        for (int b = a + 1; b < items.size(); ++b)
            QVERIFY2(!rectIn(items[a], d).intersects(rectIn(items[b], d)),
                     qPrintable(tag + " " + items[a]->objectName() + " x " + items[b]->objectName()));
    auto* results = d->findChild<QScrollArea*>("areaResultado");
    auto* options = d->findChild<QScrollArea*>("areaOpcoes");
    QVERIFY2(results && options && results->isVisible() && options->isVisible(), t);
    QVERIFY2(!rectIn(results, d).intersects(rectIn(options, d)), t);
    QVERIFY2(results->height() >= results->minimumHeight() && options->height() >= options->minimumHeight(), t);
    // Textos com quebra de linha: altura suficiente para a largura atribuída (sem corte).
    for (QLabel* l : d->findChildren<QLabel*>())
        if (l->isVisible() && l->wordWrap() && !l->text().isEmpty() && l->objectName() != "resultadoDiscretizacao")
            QVERIFY2(l->height() >= l->heightForWidth(l->width()), qPrintable(tag + " " + l->objectName()));
    // Conteúdo das opções: largura igual à da área (sem corte horizontal) e controles inteiros.
    QWidget* content = options->widget();
    QVERIFY2(content->width() <= options->viewport()->width(), t);
    for (QWidget* w : content->findChildren<QWidget*>())
        if (w->isVisible() && (qobject_cast<QCheckBox*>(w) || qobject_cast<QComboBox*>(w)))
            QVERIFY2(w->width() >= w->minimumSizeHint().width(), qPrintable(tag + " " + w->objectName()));
}

void TestE57231Ui::geometriaInicialPorResolucao()
{
    DiscretizationDialog d({{"op-1", "Placa"}}, "op-1", Parameters{}, LengthUnit::Millimeter, {});
    const QSize preferred = d.preferredSize();
    const QSize minimum = d.minimumSizeHint().expandedTo(d.minimumSize());
    std::printf("tamanho preferido %dx%d; minimo do layout %dx%d\n", preferred.width(), preferred.height(),
                minimum.width(), minimum.height());
    for (const Screen& s : kScreens)
    {
        const QRect avail = s.available();
        const QMargins frame(8, 31, 8, 8); // moldura típica do Windows 10 em pixels lógicos
        const QRect g = DiscretizationDialog::fitGeometry(preferred, minimum, avail, frame);
        const QRect outer = g.marginsAdded(frame);
        std::printf("%-15s area util %4dx%4d -> janela %4dx%4d em (%d,%d); moldura %4dx%4d = %.1f%% da altura\n",
                    qPrintable(s.name()), avail.width(), avail.height(), g.width(), g.height(), g.x(), g.y(),
                    outer.width(), outer.height(), 100.0 * outer.height() / avail.height());
        const QByteArray nameBytes = s.name().toLocal8Bit();
        const char* t = nameBytes.constData();
        QVERIFY2(avail.contains(outer), t);                                  // dentro da área útil
        QVERIFY2(outer.height() <= int(avail.height() * 0.90) + 1, t);       // ~90 % da altura útil
        QVERIFY2(outer.width() <= int(avail.width() * 0.95) + 1, t);
        QVERIFY2(g.width() >= minimum.width() && g.height() >= minimum.height(), t);
        QVERIFY2(std::abs((outer.left() - avail.left()) - (avail.right() - outer.right())) <= 1, t); // centralizada
        QVERIFY2(std::abs((outer.top() - avail.top()) - (avail.bottom() - outer.bottom())) <= 1, t);
    }
    // Tela menor que o mínimo do layout: a barra de título continua dentro da área útil.
    const QRect tiny(0, 0, 400, 300);
    const QRect g = DiscretizationDialog::fitGeometry(preferred, minimum, tiny, QMargins(8, 31, 8, 8));
    QVERIFY(g.top() - 31 >= tiny.top() && g.left() - 8 >= tiny.left());
}

void TestE57231Ui::layoutPorResolucao()
{
    DiscretizationDialog d({{"op-1", "Placa (op-1)"}}, "op-1", Parameters{}, LengthUnit::Millimeter, {});
    d.setAttribute(Qt::WA_DontShowOnScreen);
    d.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d) || d.isVisible());
    // Redimensionável: sem tamanho fixo e com alça de redimensionamento.
    QVERIFY(d.minimumSize() != d.maximumSize());
    QVERIFY(d.isSizeGripEnabled());
    auto* results = d.findChild<QScrollArea*>("areaResultado");
    QVERIFY(results);
    const QString evidence = qEnvironmentVariable("CADMECANICO_EVIDENCE_DIR");
    const QSize minimum = d.minimumSizeHint().expandedTo(d.minimumSize());

    for (int pass = 0; pass < 2; ++pass)
    {
        // pass 0: sem resultado; pass 1: resultado longo (projeção com muitas linhas).
        if (pass == 1)
            d.setResult(longResult(), "op-1", d.parameters());
        for (const Screen& s : kScreens)
        {
            const QRect g =
                DiscretizationDialog::fitGeometry(d.preferredSize(), minimum, s.available(), QMargins(8, 31, 8, 8));
            d.resize(g.size());
            QCoreApplication::processEvents();
            d.layout()->activate();
            QCoreApplication::processEvents();
            QCOMPARE(d.size(), g.size()); // o conteúdo não força uma janela maior
            const QString tag = s.name() + (pass ? "_com_resultado" : "");
            verifyLayout(&d, tag);
            if (QTest::currentTestFailed())
                return;
            if (pass == 1)
            {
                // Rolagem vertical dos resultados: barra ativa e percorre até a última linha.
                QScrollBar* bar = results->verticalScrollBar();
                QVERIFY2(bar->maximum() > 0 && bar->isVisible(), qPrintable(tag));
                bar->setValue(bar->maximum());
                QCOMPARE(bar->value(), bar->maximum());
                bar->setValue(0);
            }
            if (!evidence.isEmpty())
                d.grab().save(evidence + "/E57231_layout_" + tag + ".png");
        }
    }
    // Redimensionamento manual: aumentar a altura amplia os resultados; reduzir até o mínimo
    // mantém os botões acessíveis.
    d.resize(800, 700);
    d.layout()->activate();
    QCoreApplication::processEvents();
    const int before = results->height();
    d.resize(800, 900);
    d.layout()->activate();
    QCoreApplication::processEvents();
    QVERIFY(results->height() > before);
    d.resize(minimum);
    d.layout()->activate();
    QCoreApplication::processEvents();
    QCOMPARE(d.size(), minimum);
    verifyLayout(&d, "tamanho_minimo");
    std::printf("tamanho minimo %dx%d: resultados com %d px de altura\n", minimum.width(), minimum.height(),
                results->height());
}

void TestE57231Ui::fluxoRealComProjecao()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TestableMainWindow window;
    const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size() * 0.85;
    window.resize(QSize(1200, 800).boundedTo(available));
    window.move(20, 20);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitViewerReady(window.viewer()));
    // Placa 20 × 20 × 1 mm com furo central passante Ø10.
    QVERIFY(window.startNewSketch(PlaneType::XY));
    QVERIFY(window.addSketchGeometry(SketchRectangle{-10, -10, 20, 20}));
    QVERIFY(window.finishSketch());
    QVERIFY(window.addExtrusion(ExtrudeParameters{"Placa", window.document().sketches().back().id(), 1.0}));
    QVERIFY(window.startNewSketch(PlaneType::XY));
    QVERIFY(window.addSketchGeometry(SketchCircle{0, 0, 5}));
    QVERIFY(window.finishSketch());
    ExtrudeParameters hole;
    hole.name = "Furo";
    hole.sketchId = window.document().sketches().back().id();
    hole.mode = cad::document::ExtrudeMode::Cut;
    hole.targetId = "op-1";
    hole.endCondition = cad::document::EndCondition::ThroughAll;
    hole.reversed = true;
    QVERIFY2(window.addExtrusion(hole), qPrintable(window.errors.join('\n')));
    window.nextNodePath = dir.filePath("placa_nodes.txt");
    window.nextApdlPath = dir.filePath("placa_PREP7.inp");

    bool reached = false;
    whenDialogOpens<DiscretizationDialog>([&](DiscretizationDialog* d) {
        const auto close = qScopeGuard([d] { if (d->isVisible()) d->reject(); });
        QScreen* screen = window.screen();
        const QRect avail = screen->availableGeometry();
        const QRect outer = d->frameGeometry();
        std::printf("monitor: %s %dx%d (fator %.2f); area util %dx%d; janela %dx%d, moldura %dx%d em (%d,%d) "
                    "= %.1f%% da altura util\n",
                    qPrintable(screen->name()), screen->geometry().width(), screen->geometry().height(),
                    screen->devicePixelRatio(), avail.width(), avail.height(), d->width(), d->height(),
                    outer.width(), outer.height(), outer.x(), outer.y(), 100.0 * outer.height() / avail.height());
        QVERIFY(avail.contains(outer));                               // inteira na área útil
        QVERIFY(outer.height() <= int(avail.height() * 0.90) + 2);    // ~90 % da altura útil
        QVERIFY(std::abs((outer.top() - avail.top()) - (avail.bottom() - outer.bottom())) <= 2); // centralizada
        QVERIFY(!d->isMaximized());
        verifyLayout(d, "monitor_atual");
        const QSize opened = d->size();
        const QString evidence = qEnvironmentVariable("CADMECANICO_EVIDENCE_DIR");
        const QString scale = QString::number(int(std::lround(screen->devicePixelRatio() * 100)));
        if (!evidence.isEmpty())
            d->grab().save(evidence + "/E57231_real_" + scale + "_01_aberta.png");

        // Geração com projeção: o resultado ganha as linhas da projeção sem aumentar a janela.
        d->findChild<QDoubleSpinBox*>("campoLco")->setValue(1.0);
        auto* criterion = d->findChild<QComboBox*>("campoCriterio");
        criterion->setCurrentIndex(criterion->findData(int(InclusionCriterion::BoundaryProjection)));
        d->findChild<QPushButton*>("botaoGerar")->click();
        QVERIFY(window.discretization() && window.discretization()->adjusted());
        QCoreApplication::processEvents();
        QVERIFY(d->findChild<QLabel*>("resultadoDiscretizacao")->text().contains("Nós projetados"));
        QCOMPARE(d->size(), opened);
        QVERIFY(avail.contains(d->frameGeometry()));
        verifyLayout(d, "monitor_atual_apos_projecao");
        if (!evidence.isEmpty())
            d->grab().save(evidence + "/E57231_real_" + scale + "_02_apos_projecao.png");

        // Exportações pelos botões.
        auto* exportAscii = d->findChild<QPushButton*>("botaoExportar");
        auto* exportApdl = d->findChild<QPushButton*>("botaoExportarApdl");
        QVERIFY(exportAscii->isEnabled() && exportApdl->isEnabled());
        exportAscii->click();
        exportApdl->click();
        const int nodes = int(window.discretization()->totalNodeCount());
        QCOMPARE(countLines(window.nextApdlPath, "N,"), nodes);
        QVERIFY(QFile::exists(window.nextNodePath));
        QVERIFY(QFileInfo(window.nextNodePath).size() > 0);

        // Redimensionamento manual da janela real.
        // Redução da altura (acima do mínimo, para não esbarrar no arredondamento do tamanho mínimo
        // de janela do Windows com escala fracionária): os resultados diminuem e os botões ficam.
        auto* results = d->findChild<QScrollArea*>("areaResultado");
        const int resultsBefore = results->height();
        d->resize(d->width(), d->minimumSizeHint().height() + 40);
        QCoreApplication::processEvents();
        auto* options = d->findChild<QScrollArea*>("areaOpcoes");
        std::printf("reducao da altura: resultados %d -> %d px (minimo %d); opcoes %d px\n", resultsBefore,
                    results->height(), results->minimumHeight(), options->height());
        QVERIFY(results->height() < resultsBefore || results->height() == results->minimumHeight());
        QVERIFY(results->verticalScrollBar()->maximum() > 0);
        verifyLayout(d, "monitor_atual_reduzida");
        d->findChild<QPushButton*>("botaoFechar")->click();
        reached = true;
    });
    window.findChild<QAction*>("actionDiscretizacao")->trigger();
    QVERIFY(reached);
    QVERIFY2(window.errors.isEmpty(), qPrintable(window.errors.join('\n')));
}

QTEST_MAIN(TestE57231Ui)
#include "test_e57231_ui.moc"
