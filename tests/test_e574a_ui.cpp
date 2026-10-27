// E5.7.4a — interface das propriedades físicas do modelo MED:
//  - menu "Análise MED" com a ação "Propriedades do Modelo", que abre a janela "Propriedades
//    Físicas — Modelo MED";
//  - campos, símbolos e unidades; CORRCAF exibido como 1 e não editável;
//  - validação na janela (vazio, texto não numérico, NaN, infinito, regras do MATLAB) sem fechar;
//  - Aplicar grava no projeto; Cancelar descarta; editar os campos não altera o projeto;
//  - persistência pelo fluxo real (salvar, novo, abrir) e abertura de projeto antigo;
//  - tamanho inicial para 1366 × 768 a 2560 × 1440 com 100 %, 125 % e 150 % (área útil lógica) e
//    layout nesses tamanhos (controles visíveis, sem sobreposição, botões fora da rolagem); no
//    monitor atual, a janela abre dentro da área útil (a escala é variada pelo CTest com
//    QT_SCALE_FACTOR).
#include "ViewerTestUtils.h"

#include "app/DemPropertiesDialog.h"
#include "app/DiscretizationDialog.h"
#include "app/MainWindow.h"
#include "document/DocumentFile.h"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cmath>
#include <functional>

using cad::app::DemPropertiesDialog;
using cad::app::DiscretizationDialog;
using cad::app::MainWindow;
using cad::dem::DemProperties;
using namespace cad::test::viewer;

namespace {

class TestableMainWindow : public MainWindow
{
public:
    QString nextSavePath;
    QString nextOpenPath;
    QStringList errors, infos;

protected:
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    SaveChoice askSaveChanges() override { return SaveChoice::Discard; }
    bool confirmOverwrite(const QString&) override { return true; }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
};

// Executa 'body' quando o diálogo modal do tipo T estiver aberto.
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

DemProperties sample()
{
    DemProperties p;
    p.material.density = 11.9;
    p.material.poissonRatio = 0.25;
    p.material.youngModulus = 4.35e6;
    p.material.fractureEnergy = 133.4;
    p.model.failureFactor = 3.89;
    return p;
}

QLineEdit* field(QWidget* d, const char* name)
{
    return d->findChild<QLineEdit*>(name);
}

void setText(QWidget* d, const char* name, const QString& text)
{
    QLineEdit* f = field(d, name);
    f->clear();
    QTest::keyClicks(f, text);
}

void fill(QWidget* d, const QString& density, const QString& poisson, const QString& young, const QString& grf,
          const QString& rfc)
{
    setText(d, "campoDensidade", density);
    setText(d, "campoPoisson", poisson);
    setText(d, "campoModuloElasticidade", young);
    setText(d, "campoEnergiaFratura", grf);
    setText(d, "campoRFC", rfc);
}

QPushButton* button(QWidget* d, QDialogButtonBox::StandardButton which)
{
    return d->findChild<QDialogButtonBox*>("botoesPropriedadesMED")->button(which);
}

// Janela principal dentro da área útil (como em test_e57231_ui) e exposta.
bool showMainWindow(QWidget& w)
{
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    w.resize(QSize(1200, 800).boundedTo(available.size() * 0.9));
    w.move(available.topLeft() + QPoint(20, 20));
    w.show();
    return QTest::qWaitForWindowExposed(&w, 15000) || w.isVisible();
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

QRect rectIn(const QWidget* w, const QWidget* root)
{
    return QRect(w->mapTo(root, QPoint(0, 0)), w->size());
}

bool insideScrollArea(const QWidget* w)
{
    for (const QWidget* p = w->parentWidget(); p; p = p->parentWidget())
        if (qobject_cast<const QScrollArea*>(p))
            return true;
    return false;
}

const char* const kFields[] = {"campoDensidade", "campoPoisson", "campoModuloElasticidade", "campoEnergiaFratura",
                               "campoRFC", "campoCORRCAF"};

} // namespace

class TestE574aUi : public QObject
{
    Q_OBJECT

private slots:
    void menuEJanela();
    void camposEUnidades();
    void validacaoNaJanela();
    void aplicarECancelar();
    void persistenciaPeloFluxoReal();
    void geometriaELayoutPorResolucao();
    void aberturaNoMonitorAtual();

private:
    void verifyLayout(DemPropertiesDialog* d, const QString& tag);
};

void TestE574aUi::menuEJanela()
{
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    auto* menu = w.findChild<QMenu*>("menuAnaliseMED");
    QVERIFY(menu);
    QCOMPARE(menu->title().remove('&'), QString("Análise MED"));
    auto* action = w.findChild<QAction*>("actionPropriedadesModeloMED");
    QVERIFY(action);
    QVERIFY(menu->actions().contains(action));
    QCOMPARE(action->text().remove('&'), QString("Propriedades do Modelo..."));
    QVERIFY(action->isEnabled()); // também em documento vazio

    QString title;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        title = d->windowTitle();
        d->reject();
    });
    action->trigger();
    QCOMPARE(title, QString("Propriedades Físicas — Modelo MED"));
    QVERIFY(!w.document().demProperties());
    QVERIFY(!w.document().isModified());
}

void TestE574aUi::camposEUnidades()
{
    DemPropertiesDialog d(std::nullopt);
    d.setAttribute(Qt::WA_DontShowOnScreen);
    d.show();
    // Grupos.
    QCOMPARE(d.findChild<QGroupBox*>("grupoMaterial")->title(), QString("Propriedades do material"));
    QCOMPARE(d.findChild<QGroupBox*>("grupoModeloMED")->title(), QString("Parâmetros do modelo MED"));
    // Campos vazios em projeto sem propriedades (nenhum valor apresentado como resultado).
    for (const char* name : {"campoDensidade", "campoPoisson", "campoModuloElasticidade", "campoEnergiaFratura", "campoRFC"})
    {
        QVERIFY2(field(&d, name) && field(&d, name)->text().isEmpty(), name);
        QVERIFY2(!field(&d, name)->isReadOnly() && field(&d, name)->isEnabled(), name);
    }
    QVERIFY(!d.properties());
    // CORRCAF: exibido como 1, não editável.
    QLineEdit* corrcaf = field(&d, "campoCORRCAF");
    QCOMPARE(corrcaf->text(), QString("1"));
    QVERIFY(corrcaf->isReadOnly() && !corrcaf->isEnabled());
    QTest::keyClicks(corrcaf, "5");
    QCOMPARE(corrcaf->text(), QString("1"));
    // Unidades junto aos campos.
    const std::pair<const char*, const char*> units[] = {
        {"unidadeDensidade", "kg/m³"},           {"unidadePoisson", "adimensional"},
        {"unidadeModuloElasticidade", "Pa (N/m²)"}, {"unidadeEnergiaFratura", "N/m (J/m²)"},
        {"unidadeRFC", "adimensional"},          {"unidadeCORRCAF", "adimensional (fixo)"},
    };
    for (const auto& [name, text] : units)
    {
        auto* l = d.findChild<QLabel*>(name);
        QVERIFY2(l && l->isVisible(), name);
        QCOMPARE(l->text(), QString::fromUtf8(text));
    }
    // Símbolos nos rótulos.
    QStringList labels;
    for (QLabel* l : d.findChildren<QLabel*>())
        labels << l->text();
    for (const char* text : {"Densidade (ρ):", "Coeficiente de Poisson (ν):", "Módulo de elasticidade (E):",
                             "Energia específica de fratura por área (GRF):", "Fator de falha (RFC):",
                             "Fator de compatibilização de malha (CORRCAF):"})
        QVERIFY2(labels.contains(QString::fromUtf8(text)), text);
    // Projeto com propriedades: valores exibidos sem perda.
    DemPropertiesDialog e(sample());
    QCOMPARE(field(&e, "campoDensidade")->text(), QString("11.9"));
    QCOMPARE(field(&e, "campoPoisson")->text(), QString("0.25"));
    QCOMPARE(field(&e, "campoModuloElasticidade")->text(), QString("4350000"));
    QCOMPARE(field(&e, "campoEnergiaFratura")->text(), QString("133.4"));
    QCOMPARE(field(&e, "campoRFC")->text(), QString("3.89"));
    QVERIFY(e.properties() && *e.properties() == sample());
}

void TestE574aUi::validacaoNaJanela()
{
    DemPropertiesDialog d(std::nullopt);
    d.setAttribute(Qt::WA_DontShowOnScreen);
    d.show();
    QPushButton* apply = button(&d, QDialogButtonBox::Ok);
    QCOMPARE(apply->text(), QString("Aplicar"));
    QCOMPARE(button(&d, QDialogButtonBox::Cancel)->text(), QString("Cancelar"));

    struct Case
    {
        QStringList values; // ρ, ν, E, GRF, RFC
        const char* focused;
        const char* message;
    };
    const Case cases[] = {
        {{"", "0.36", "3.5e9", "2400", "10"}, "campoDensidade", "informe um valor"},
        {{"abc", "0.36", "3.5e9", "2400", "10"}, "campoDensidade", "não é um número válido"},
        {{"1240", "nan", "3.5e9", "2400", "10"}, "campoPoisson", "não é um número válido"},
        {{"1240", "0.36", "inf", "2400", "10"}, "campoModuloElasticidade", "não é um número válido"},
        {{"1240", "0.36", "3.5e9", "1e999", "10"}, "campoEnergiaFratura", "não é um número válido"},
        {{"1240", "0.36", "3.5e9", "2400", "1,0.5"}, "campoRFC", "não é um número válido"},
        {{"-1240", "0.36", "3.5e9", "2400", "10"}, "campoDensidade", "densidade"},
        {{"1240", "0.5", "3.5e9", "2400", "10"}, "campoPoisson", "(−1; 0,5)"},
        {{"1240", "-1", "3.5e9", "2400", "10"}, "campoPoisson", "(−1; 0,5)"},
        {{"1240", "0.36", "0", "2400", "10"}, "campoModuloElasticidade", "módulo de elasticidade"},
        {{"1240", "0.36", "3.5e9", "0", "10"}, "campoEnergiaFratura", "GRF"},
        {{"1240", "0.36", "3.5e9", "2400", "0"}, "campoRFC", "RFC"},
        {{"1240", "0.36", "3.5e9", "2400", "-10"}, "campoRFC", "RFC"},
    };
    for (const Case& c : cases)
    {
        fill(&d, c.values[0], c.values[1], c.values[2], c.values[3], c.values[4]);
        QTest::mouseClick(apply, Qt::LeftButton);
        QVERIFY2(d.isVisible() && d.result() != QDialog::Accepted, c.focused); // permanece aberto
        QVERIFY2(d.validationMessage().contains(QString::fromUtf8(c.message), Qt::CaseInsensitive),
                 qPrintable(c.values.join(" | ") + " -> " + d.validationMessage()));
        QCOMPARE(d.focusWidget() ? d.focusWidget()->objectName() : QString(), QString(c.focused));
    }
    // Valores padrão do MATLAB: preenchem os campos (não aplicam) e são válidos.
    QTest::mouseClick(d.findChild<QPushButton*>("botaoPadraoMATLAB"), Qt::LeftButton);
    QVERIFY(d.isVisible());
    QVERIFY(d.validationMessage().isEmpty());
    QVERIFY(d.properties() && *d.properties() == cad::dem::matlabDefaults());
    // Vírgula decimal aceita.
    fill(&d, "1240", "0,36", "3,5e9", "2400", "10");
    QTest::mouseClick(apply, Qt::LeftButton);
    QCOMPARE(d.result(), int(QDialog::Accepted));
    QVERIFY(d.properties() && *d.properties() == cad::dem::matlabDefaults());
}

void TestE574aUi::aplicarECancelar()
{
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    w.newExampleDocument();
    QVERIFY(!w.document().isModified());
    const int revision = w.document().revision();
    auto* action = w.findChild<QAction*>("actionPropriedadesModeloMED");

    // Cancelar descarta; editar os campos não altera o projeto.
    bool unchangedWhileEditing = false;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        fill(d, "11.9", "0.25", "4.35e6", "133.4", "3.89");
        unchangedWhileEditing = !w.document().demProperties() && !w.document().isModified();
        QTest::mouseClick(button(d, QDialogButtonBox::Cancel), Qt::LeftButton);
    });
    action->trigger();
    QVERIFY(unchangedWhileEditing);
    QVERIFY(!w.document().demProperties());
    QVERIFY(!w.document().isModified());

    // Aplicar com valor inválido mantém a janela aberta e o projeto inalterado; corrigido, grava.
    bool keptOpen = false;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        fill(d, "11.9", "0.7", "4.35e6", "133.4", "3.89");
        QTest::mouseClick(button(d, QDialogButtonBox::Ok), Qt::LeftButton);
        keptOpen = d->isVisible() && !w.document().demProperties();
        setText(d, "campoPoisson", "0.25");
        QTest::mouseClick(button(d, QDialogButtonBox::Ok), Qt::LeftButton);
    });
    action->trigger();
    QVERIFY(keptOpen);
    QVERIFY(w.document().demProperties());
    QVERIFY(*w.document().demProperties() == sample());
    QCOMPARE(w.document().demProperties()->model.corrcaf(), 1.0);
    QVERIFY(w.document().isModified());
    QVERIFY(w.isWindowModified());
    QCOMPARE(w.document().revision(), revision); // geometria não recalculada
    QVERIFY(w.errors.isEmpty());

    // Reabrir a janela mostra os valores aplicados; Cancelar mantém o projeto.
    QString shownDensity;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        shownDensity = field(d, "campoDensidade")->text();
        setText(d, "campoDensidade", "999");
        d->reject();
    });
    action->trigger();
    QCOMPARE(shownDensity, QString("11.9"));
    QCOMPARE(w.document().demProperties()->material.density, 11.9);

    // applyDemProperties recusa valores inválidos com mensagem e sem alterar o projeto.
    DemProperties bad = sample();
    bad.material.youngModulus = -1.0;
    QVERIFY(!w.applyDemProperties(bad));
    QCOMPARE(w.errors.size(), 1);
    QVERIFY(*w.document().demProperties() == sample());
}

void TestE574aUi::persistenciaPeloFluxoReal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));

    // Projeto A com propriedades; projeto B com outras.
    w.newExampleDocument();
    QVERIFY(w.applyDemProperties(sample()));
    w.nextSavePath = dir.filePath("A.cadmec");
    QVERIFY(w.saveDocumentAs());
    w.newExampleDocument();
    QVERIFY(!w.document().demProperties()); // novo projeto não herda as propriedades
    QVERIFY(w.applyDemProperties(cad::dem::matlabDefaults()));
    w.nextSavePath = dir.filePath("B.cadmec");
    QVERIFY(w.saveDocumentAs());

    w.newDocument();
    QVERIFY(!w.document().demProperties());
    w.nextOpenPath = dir.filePath("A.cadmec");
    w.openDocument();
    QCOMPARE(w.documentPath(), dir.filePath("A.cadmec"));
    QVERIFY(w.document().demProperties() && *w.document().demProperties() == sample());
    QVERIFY(!w.document().isModified());
    // A janela mostra os valores recuperados.
    QString shown;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        shown = field(d, "campoRFC")->text();
        d->reject();
    });
    w.findChild<QAction*>("actionPropriedadesModeloMED")->trigger();
    QCOMPARE(shown, QString("3.89"));

    w.nextOpenPath = dir.filePath("B.cadmec");
    w.openDocument();
    QVERIFY(w.document().demProperties() && *w.document().demProperties() == cad::dem::matlabDefaults());

    // Projeto antigo (esquema 6): abre sem propriedades; a janela mostra campos vazios.
    w.nextOpenPath = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e55_v6.cadmec");
    w.openDocument();
    QVERIFY2(w.errors.isEmpty(), qPrintable(w.errors.join("\n")));
    QVERIFY(!w.document().operations().empty());
    QVERIFY(!w.document().demProperties());
    bool empty = false;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        empty = field(d, "campoDensidade")->text().isEmpty() && field(d, "campoCORRCAF")->text() == "1";
        d->reject();
    });
    w.findChild<QAction*>("actionPropriedadesModeloMED")->trigger();
    QVERIFY(empty);
}

void TestE574aUi::verifyLayout(DemPropertiesDialog* d, const QString& tag)
{
    const QByteArray tagBytes = tag.toLocal8Bit();
    const char* t = tagBytes.constData();
    const QRect area = d->rect();
    // Botões: visíveis, inteiros dentro da janela, texto sem corte e fora da rolagem.
    auto* box = d->findChild<QDialogButtonBox*>("botoesPropriedadesMED");
    QVERIFY2(box && box->isVisible() && !insideScrollArea(box), t);
    for (QPushButton* b : box->findChildren<QPushButton*>())
    {
        QVERIFY2(b->isVisible() && area.contains(rectIn(b, d)), qPrintable(tag + " " + b->text()));
        QVERIFY2(b->width() >= b->sizeHint().width(), qPrintable(tag + " " + b->text()));
    }
    // Campos: inteiros, largura mínima respeitada, sem sobreposição; com rolagem, acessíveis.
    auto* scroll = d->findChild<QScrollArea*>("areaPropriedadesMED");
    QVERIFY2(scroll && scroll->isVisible() && !rectIn(scroll, d).intersects(rectIn(box, d)), t);
    QVERIFY2(scroll->widget()->width() <= scroll->viewport()->width(), t); // sem corte horizontal
    QList<QWidget*> items;
    for (const char* name : kFields)
    {
        QLineEdit* f = field(d, name);
        QVERIFY2(f && f->isVisibleTo(d), qPrintable(tag + " " + name));
        QVERIFY2(f->width() >= f->minimumSizeHint().width() && f->height() >= f->minimumSizeHint().height(),
                 qPrintable(tag + " " + name));
        items << f;
    }
    for (QLabel* l : d->findChildren<QLabel*>())
    {
        if (!l->isVisibleTo(d) || l->text().isEmpty() || l->objectName() == "mensagemValidacao")
            continue;
        items << l;
        if (l->wordWrap())
            QVERIFY2(l->height() >= l->heightForWidth(l->width()), qPrintable(tag + " " + l->objectName()));
        else
            QVERIFY2(l->width() >= l->sizeHint().width(), qPrintable(tag + " " + l->text()));
    }
    QWidget* content = scroll->widget();
    for (int a = 0; a < items.size(); ++a)
        for (int b = a + 1; b < items.size(); ++b)
            QVERIFY2(!rectIn(items[a], content).intersects(rectIn(items[b], content)),
                     qPrintable(tag + " " + items[a]->objectName() + " x " + items[b]->objectName()));
    // Cada campo pode ser trazido à vista pela rolagem.
    for (const char* name : kFields)
    {
        scroll->ensureWidgetVisible(field(d, name));
        const QRect r = rectIn(field(d, name), scroll->viewport());
        QVERIFY2(scroll->viewport()->rect().contains(r), qPrintable(tag + " rolagem " + name));
    }
}

void TestE574aUi::geometriaELayoutPorResolucao()
{
    DemPropertiesDialog d(sample());
    d.setAttribute(Qt::WA_DontShowOnScreen);
    d.show();
    const QSize preferred = d.preferredSize();
    const QSize minimum = d.minimumSizeHint().expandedTo(d.minimumSize());
    std::printf("tamanho preferido %dx%d; minimo do layout %dx%d\n", preferred.width(), preferred.height(),
                minimum.width(), minimum.height());
    for (const Screen& s : kScreens)
    {
        const QRect avail = s.available();
        const QMargins frame(8, 31, 8, 8);
        const QRect g = DiscretizationDialog::fitGeometry(preferred, minimum, avail, frame);
        const QRect outer = g.marginsAdded(frame);
        QVERIFY2(avail.contains(outer), qPrintable(s.name()));
        QVERIFY2(outer.height() <= int(avail.height() * 0.90) + 1, qPrintable(s.name()));
        QVERIFY2(std::abs(outer.center().x() - avail.center().x()) <= 1, qPrintable(s.name()));
        std::printf("%s: area util %dx%d -> janela %dx%d em (%d,%d)\n", qPrintable(s.name()), avail.width(),
                    avail.height(), g.width(), g.height(), g.x(), g.y());
        d.resize(g.size());
        QApplication::processEvents();
        verifyLayout(&d, s.name());
    }
    // Redimensionamento manual até o mínimo do layout: controles continuam acessíveis.
    d.resize(minimum);
    QApplication::processEvents();
    verifyLayout(&d, "minimo");
    d.resize(preferred.width() + 300, preferred.height() + 200);
    QApplication::processEvents();
    verifyLayout(&d, "ampliada");
}

void TestE574aUi::aberturaNoMonitorAtual()
{
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    QRect frame, avail;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        QApplication::processEvents();
        frame = d->frameGeometry();
        avail = d->screen()->availableGeometry();
        verifyLayout(d, "monitor atual");
        d->reject();
    });
    w.findChild<QAction*>("actionPropriedadesModeloMED")->trigger();
    std::printf("monitor: area util %dx%d (escala %.2f); janela com moldura %dx%d em (%d,%d)\n", avail.width(),
                avail.height(), w.screen()->devicePixelRatio(), frame.width(), frame.height(), frame.x(), frame.y());
    QVERIFY(!frame.isEmpty());
    QVERIFY2(avail.contains(frame), "janela fora da area util");
    QVERIFY(frame.height() <= int(avail.height() * 0.90) + 2);
}

QTEST_MAIN(TestE574aUi)
#include "test_e574a_ui.moc"
