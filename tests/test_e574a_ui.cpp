// E5.7.4a e E5.7.4a.1 — interface das propriedades físicas do modelo MED:
//  - menu "Análise MED" com a ação "Propriedades do Modelo", que abre a janela "Propriedades
//    Físicas — Modelo MED";
//  - campos, símbolos e unidades; CORRCAF exibido como 1 e não editável; d_eq [m] editável;
//  - RFC, POA e EPR somente leitura, recalculados a cada edição e "—" com entradas inválidas;
//  - validação na janela (vazio, texto não numérico, NaN, infinito, regras do MATLAB) sem fechar;
//  - Aplicar grava no projeto; Cancelar descarta; editar os campos não altera o projeto;
//  - persistência pelo fluxo real (salvar, novo, abrir) e abertura de projeto antigo;
//  - projeto de esquema 10 (RFC): abre com a geometria, propriedades MED não definidas e aviso;
//    Salvar sobre ele sem redefinir as propriedades pede confirmação (Cancelar é o padrão);
//    Salvar como em outro caminho preserva o original e só informa;
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
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QMessageBox>
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
    SaveChoice saveChoice = SaveChoice::Discard;
    // Confirmação de sobrescrita do arquivo de esquema 10 (E5.7.4a.1): resposta simulada, ou o
    // diálogo real quando 'realLegacyDialog'.
    int legacyConfirmations = 0;
    bool legacyAnswer = false;
    bool realLegacyDialog = false;
    // Confirmação de substituição de arquivo existente (Salvar como): contada, resposta simulada.
    int overwriteConfirmations = 0;
    bool overwriteAnswer = true;

protected:
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    SaveChoice askSaveChanges() override { return saveChoice; }
    bool confirmOverwrite(const QString&) override
    {
        ++overwriteConfirmations;
        return overwriteAnswer;
    }
    void reportError(const QString&, const QString& text) override { errors << text; }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
    bool confirmDiscardLegacyDem(const QString& filePath) override
    {
        ++legacyConfirmations;
        return realLegacyDialog ? MainWindow::confirmDiscardLegacyDem(filePath) : legacyAnswer;
    }
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
    p.model.equivalentDefect = 0.0066;
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
          const QString& deq)
{
    setText(d, "campoDensidade", density);
    setText(d, "campoPoisson", poisson);
    setText(d, "campoModuloElasticidade", young);
    setText(d, "campoEnergiaFratura", grf);
    setText(d, "campoDeq", deq);
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// Arquivo de esquema 10 (E5.7.4a) válido: peça de teste com propriedades MED no formato antigo (RFC).
QByteArray schema10File()
{
    cad::document::CadDocument doc = cad::document::CadDocument::withTestPart("Peça antiga");
    doc.setDemProperties(sample());
    QJsonObject root = QJsonDocument::fromJson(cad::document::DocumentFile::toJson(doc)).object();
    root["schemaVersion"] = 10;
    QJsonObject document = root["document"].toObject();
    QJsonObject dem = document["demProperties"].toObject();
    dem["model"] = QJsonObject{{"failureFactor", 10.0}, {"corrcaf", 1.0}};
    document["demProperties"] = dem;
    root["document"] = document;
    return QJsonDocument(root).toJson();
}

QJsonObject jsonOf(const QString& path)
{
    return QJsonDocument::fromJson(readBytes(path)).object();
}

// Valor numérico de um campo somente leitura (NaN se "—" ou vazio).
double shownValue(QWidget* d, const char* name)
{
    bool ok = false;
    const double v = field(d, name)->text().toDouble(&ok);
    return ok ? v : std::nan("");
}

bool approx(double value, double expected, double relative)
{
    return std::fabs(value - expected) <= relative * std::fabs(expected);
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
                               "campoDeq",       "campoCORRCAF", "campoRFC",                "campoPOA",
                               "campoEPR"};
const char* const kDerived[] = {"campoRFC", "campoPOA", "campoEPR"};

} // namespace

class TestE574aUi : public QObject
{
    Q_OBJECT

private slots:
    void menuEJanela();
    void camposEUnidades();
    void grandezasCalculadas();
    void validacaoNaJanela();
    void aplicarECancelar();
    void persistenciaPeloFluxoReal();
    void projetoEsquema10();
    void salvarEsquema10Cancelar();
    void salvarEsquema10Confirmar();
    void salvarComoEsquema10();
    void cancelarSalvamentoAoAbrirOutro();
    void salvarComoGrafiasEquivalentes();
    void salvarComoGrafiasEquivalentesEsquema10();
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
    QCOMPARE(d.findChild<QGroupBox*>("grupoGrandezasCalculadas")->title(), QString("Grandezas calculadas"));
    // Campos vazios em projeto sem propriedades (nenhum valor apresentado como resultado).
    for (const char* name : {"campoDensidade", "campoPoisson", "campoModuloElasticidade", "campoEnergiaFratura", "campoDeq"})
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
    // RFC, POA e EPR: somente leitura e "—" sem propriedades definidas.
    for (const char* name : kDerived)
    {
        QLineEdit* f = field(&d, name);
        QVERIFY2(f && f->isReadOnly() && !f->isEnabled(), name);
        QCOMPARE(f->text(), QString("—"));
        QTest::keyClicks(f, "5");
        QCOMPARE(f->text(), QString("—"));
        QVERIFY2(!f->toolTip().isEmpty(), name);
    }
    // Unidades junto aos campos.
    const std::pair<const char*, const char*> units[] = {
        {"unidadeDensidade", "kg/m³"},           {"unidadePoisson", "adimensional"},
        {"unidadeModuloElasticidade", "Pa (N/m²)"}, {"unidadeEnergiaFratura", "N/m (J/m²)"},
        {"unidadeDeq", "m"},                     {"unidadeCORRCAF", "adimensional (fixo)"},
        {"unidadeRFC", "m^(-1/2)"},              {"unidadePOA", "adimensional"},
        {"unidadeEPR", "adimensional"},
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
                             "Energia específica de fratura por área (GRF):",
                             "Defeito intrínseco equivalente (d_eq):", "Fator de falha (RFC = 1/√d_eq):",
                             "POA = 1 − ν²:", "Deformação de pico de referência (EPR):",
                             "Fator de compatibilização de malha (CORRCAF):"})
        QVERIFY2(labels.contains(QString::fromUtf8(text)), text);
    // Projeto com propriedades: valores exibidos sem perda.
    DemPropertiesDialog e(sample());
    QCOMPARE(field(&e, "campoDensidade")->text(), QString("11.9"));
    QCOMPARE(field(&e, "campoPoisson")->text(), QString("0.25"));
    QCOMPARE(field(&e, "campoModuloElasticidade")->text(), QString("4350000"));
    QCOMPARE(field(&e, "campoEnergiaFratura")->text(), QString("133.4"));
    QCOMPARE(field(&e, "campoDeq")->text(), QString("0.0066"));
    QVERIFY(e.properties() && *e.properties() == sample());
    // Grandezas derivadas do projeto exibidas (valores conferidos em grandezasCalculadas).
    for (const char* name : kDerived)
        QVERIFY2(std::isfinite(shownValue(&e, name)), name);
}

void TestE574aUi::grandezasCalculadas()
{
    DemPropertiesDialog d(std::nullopt);
    d.setAttribute(Qt::WA_DontShowOnScreen);
    d.show();
    // Valores padrão do MATLAB: d_eq = 0,01 m -> RFC = 10 m^(-1/2), POA = 0,8704, EPR ≈ 8,876e-3.
    QTest::mouseClick(d.findChild<QPushButton*>("botaoPadraoMATLAB"), Qt::LeftButton);
    QCOMPARE(field(&d, "campoDeq")->text(), QString("0.01"));
    QVERIFY(approx(shownValue(&d, "campoRFC"), 10.0, 1e-14));
    QVERIFY(approx(shownValue(&d, "campoPOA"), 0.8704, 1e-14));
    QVERIFY(approx(shownValue(&d, "campoEPR"), 8.876e-3, 1e-4));
    QVERIFY(approx(shownValue(&d, "campoEPR"), std::sqrt(2400.0 / (0.01 * 3.5e9 * 0.8704)), 1e-14));
    QCOMPARE(field(&d, "campoCORRCAF")->text(), QString("1"));

    // Recalculadas a cada edição: d_eq × 4 -> RFC e EPR pela metade; ν = 0 -> POA = 1; GRF × 4 -> EPR × 2.
    setText(&d, "campoDeq", "0.04");
    QVERIFY(approx(shownValue(&d, "campoRFC"), 5.0, 1e-14));
    QVERIFY(approx(shownValue(&d, "campoEPR"), std::sqrt(2400.0 / (0.04 * 3.5e9 * 0.8704)), 1e-14));
    setText(&d, "campoPoisson", "0");
    QCOMPARE(shownValue(&d, "campoPOA"), 1.0);
    QVERIFY(approx(shownValue(&d, "campoEPR"), std::sqrt(2400.0 / (0.04 * 3.5e9)), 1e-14));
    setText(&d, "campoEnergiaFratura", "9600");
    QVERIFY(approx(shownValue(&d, "campoEPR"), 2.0 * std::sqrt(2400.0 / (0.04 * 3.5e9)), 1e-14));

    // Entrada vazia, não numérica ou fora das regras: "—" nos três campos.
    const std::pair<const char*, const char*> invalid[] = {
        {"campoDeq", ""},     {"campoDeq", "abc"},           {"campoDeq", "0"},
        {"campoDeq", "-0.01"}, {"campoPoisson", "0.5"},       {"campoModuloElasticidade", "0"},
        {"campoEnergiaFratura", "-1"}, {"campoDensidade", ""},
    };
    for (const auto& [name, text] : invalid)
    {
        fill(&d, "1240", "0.36", "3.5e9", "2400", "0.01");
        QVERIFY(approx(shownValue(&d, "campoRFC"), 10.0, 1e-14));
        setText(&d, name, text);
        for (const char* derived : kDerived)
            QVERIFY2(field(&d, derived)->text() == QString("—"), qPrintable(QString("%1=%2").arg(name, text)));
    }
    // Corrigida a entrada inválida, os campos voltam a ser calculados.
    setText(&d, "campoDensidade", "1240");
    QVERIFY(approx(shownValue(&d, "campoRFC"), 10.0, 1e-14));
    // Os campos calculados não alteram as propriedades retornadas.
    fill(&d, "1240", "0.36", "3.5e9", "2400", "0.01");
    QVERIFY(d.properties() && *d.properties() == cad::dem::matlabDefaults());
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
        QStringList values; // ρ, ν, E, GRF, d_eq
        const char* focused;
        const char* message;
    };
    const Case cases[] = {
        {{"", "0.36", "3.5e9", "2400", "0.01"}, "campoDensidade", "informe um valor"},
        {{"abc", "0.36", "3.5e9", "2400", "0.01"}, "campoDensidade", "não é um número válido"},
        {{"1240", "nan", "3.5e9", "2400", "0.01"}, "campoPoisson", "não é um número válido"},
        {{"1240", "0.36", "inf", "2400", "0.01"}, "campoModuloElasticidade", "não é um número válido"},
        {{"1240", "0.36", "3.5e9", "1e999", "0.01"}, "campoEnergiaFratura", "não é um número válido"},
        {{"1240", "0.36", "3.5e9", "2400", "1,0.5"}, "campoDeq", "não é um número válido"},
        {{"1240", "0.36", "3.5e9", "2400", ""}, "campoDeq", "informe um valor"},
        {{"-1240", "0.36", "3.5e9", "2400", "0.01"}, "campoDensidade", "densidade"},
        {{"1240", "0.5", "3.5e9", "2400", "0.01"}, "campoPoisson", "(−1; 0,5)"},
        {{"1240", "-1", "3.5e9", "2400", "0.01"}, "campoPoisson", "(−1; 0,5)"},
        {{"1240", "0.36", "0", "2400", "0.01"}, "campoModuloElasticidade", "módulo de elasticidade"},
        {{"1240", "0.36", "3.5e9", "0", "0.01"}, "campoEnergiaFratura", "GRF"},
        {{"1240", "0.36", "3.5e9", "2400", "0"}, "campoDeq", "d_eq"},
        {{"1240", "0.36", "3.5e9", "2400", "-0.01"}, "campoDeq", "d_eq"},
        {{"1240", "0.36", "1e-300", "2400", "1e-30"}, "campoDeq", "não é representável"},
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
    fill(&d, "1240", "0,36", "3,5e9", "2400", "0,01");
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
        fill(d, "11.9", "0.25", "4.35e6", "133.4", "0.0066");
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
        fill(d, "11.9", "0.7", "4.35e6", "133.4", "0.0066");
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
        shown = field(d, "campoDeq")->text();
        d->reject();
    });
    w.findChild<QAction*>("actionPropriedadesModeloMED")->trigger();
    QCOMPARE(shown, QString("0.0066"));

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
    QVERIFY(w.infos.isEmpty()); // projetos sem propriedades MED antigas não geram aviso
}

void TestE574aUi::projetoEsquema10()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));

    // Arquivo de esquema 10 (E5.7.4a): propriedades MED com RFC, gerado a partir do formato atual.
    w.newExampleDocument();
    QVERIFY(w.applyDemProperties(sample()));
    const auto operations = w.document().operations().size();
    QJsonObject root = QJsonDocument::fromJson(cad::document::DocumentFile::toJson(w.document())).object();
    root["schemaVersion"] = 10;
    QJsonObject doc = root["document"].toObject();
    QJsonObject dem = doc["demProperties"].toObject();
    dem["model"] = QJsonObject{{"failureFactor", 10.0}, {"corrcaf", 1.0}};
    doc["demProperties"] = dem;
    root["document"] = doc;
    const QString path = dir.filePath("v10.cadmec");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
    }

    w.newDocument();
    auto* diagnostics = w.findChild<QDockWidget*>("dockDiagnostico");
    QVERIFY(diagnostics);
    const bool diagnosticsVisible = diagnostics->isVisible();
    w.nextOpenPath = path;
    w.openDocument();
    QCOMPARE(w.documentPath(), path);
    QVERIFY2(w.errors.isEmpty(), qPrintable(w.errors.join("; ")));
    QCOMPARE(w.document().operations().size(), operations); // geometria preservada
    QVERIFY(!w.document().demProperties());                 // não convertidas nem substituídas
    QVERIFY(!w.document().isModified());
    // Aviso pedindo a redefinição, que não é tratado como erro de operação.
    QCOMPARE(w.infos.size(), 1);
    QVERIFY(w.infos[0].contains("redefina", Qt::CaseInsensitive) && w.infos[0].contains("d_eq"));
    QCOMPARE(diagnostics->isVisible(), diagnosticsVisible);
    // A janela abre com campos vazios (nenhum valor padrão silencioso) e "—" nas derivadas.
    bool empty = false;
    whenDialogOpens<DemPropertiesDialog>([&](DemPropertiesDialog* d) {
        empty = field(d, "campoDeq")->text().isEmpty() && field(d, "campoDensidade")->text().isEmpty()
                && field(d, "campoRFC")->text() == QString("—");
        d->reject();
    });
    w.findChild<QAction*>("actionPropriedadesModeloMED")->trigger();
    QVERIFY(empty);

    // Teste 7 — redefinidas e salvas: sem confirmação; o arquivo passa ao esquema 11 com d_eq e sem RFC.
    QVERIFY(w.applyDemProperties(cad::dem::matlabDefaults()));
    QVERIFY(w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 0);
    QCOMPARE(w.infos.size(), 1); // nenhuma mensagem além do aviso de abertura
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const QJsonObject savedRoot = QJsonDocument::fromJson(saved.readAll()).object();
    QCOMPARE(savedRoot["schemaVersion"].toInt(), 11);
    const QJsonObject model = savedRoot["document"].toObject()["demProperties"].toObject()["model"].toObject();
    QCOMPARE(model["equivalentDefect"].toDouble(), 0.01);
    QVERIFY(!model.contains("failureFactor"));
}

// Teste 3 — Salvar sobre o arquivo de esquema 10 sem redefinir as propriedades: cancelar não
// altera o arquivo nem o estado do documento (resposta simulada e diálogo real).
void TestE574aUi::salvarEsquema10Cancelar()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("antigo.cadmec");
    const QByteArray original = schema10File();
    QVERIFY(writeBytes(path, original));
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    w.nextOpenPath = path;
    w.openDocument();
    QCOMPARE(w.documentPath(), path);
    QVERIFY(!w.document().demProperties());

    w.legacyAnswer = false;
    QVERIFY(!w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 1);
    QCOMPARE(readBytes(path), original); // arquivo intacto
    QCOMPARE(w.documentPath(), path);
    QVERIFY(!w.document().isModified() && !w.document().demProperties());

    // Diálogo real: botões "Cancelar" (padrão e Esc) e "Continuar e salvar".
    w.realLegacyDialog = true;
    QString defaultText;
    QStringList buttons;
    whenDialogOpens<QMessageBox>([&](QMessageBox* box) {
        if (box->defaultButton())
            defaultText = box->defaultButton()->text();
        for (QAbstractButton* b : box->buttons())
            buttons << b->text();
        QTest::keyClick(box, Qt::Key_Escape);
    });
    QVERIFY(!w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 2);
    QCOMPARE(defaultText, QString("Cancelar"));
    QVERIFY2(buttons.size() == 2 && buttons.contains("Cancelar") && buttons.contains("Continuar e salvar"),
             qPrintable(buttons.join(" | ")));
    QCOMPARE(readBytes(path), original);
    QVERIFY(!w.document().isModified() && !w.document().demProperties());
    QVERIFY(w.errors.isEmpty());

    // Enter aciona o botão padrão (Cancelar): o arquivo continua intacto.
    whenDialogOpens<QMessageBox>([&](QMessageBox* box) { QTest::keyClick(box, Qt::Key_Return); });
    QVERIFY(!w.saveDocument());
    QCOMPARE(readBytes(path), original);
}

// Teste 4 — Salvar confirmado: o arquivo passa ao esquema 11 sem propriedades MED; depois disso
// não há nova confirmação.
void TestE574aUi::salvarEsquema10Confirmar()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("antigo.cadmec");
    QVERIFY(writeBytes(path, schema10File()));
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    w.nextOpenPath = path;
    w.openDocument();
    const auto operations = w.document().operations().size();

    w.realLegacyDialog = true; // pelo diálogo real, clicando "Continuar e salvar"
    whenDialogOpens<QMessageBox>([](QMessageBox* box) {
        for (QAbstractButton* b : box->buttons())
            if (b->text() == "Continuar e salvar")
                QTest::mouseClick(b, Qt::LeftButton);
    });
    QVERIFY(w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 1);
    const QJsonObject root = jsonOf(path);
    QCOMPARE(root["schemaVersion"].toInt(), 11);
    QVERIFY(!root["document"].toObject().contains("demProperties"));
    QVERIFY(!w.document().isModified());

    // O arquivo regravado abre sem aviso, com a mesma geometria; salvar de novo não pergunta.
    QVERIFY(w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 1);
    const int infos = w.infos.size();
    w.openDocument();
    QCOMPARE(w.infos.size(), infos);
    QCOMPARE(w.document().operations().size(), operations);
    QVERIFY(!w.document().demProperties());
    QVERIFY(w.errors.isEmpty());
}

// Testes 5, 6 e 8 — Salvar como: outro caminho preserva o original e só informa; o mesmo caminho
// (inclusive com maiúsculas/minúsculas diferentes) exige confirmação; esquema 11 nunca pergunta.
void TestE574aUi::salvarComoEsquema10()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("antigo.cadmec");
    const QByteArray original = schema10File();
    QVERIFY(writeBytes(path, original));
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    w.nextOpenPath = path;
    w.openDocument();
    QCOMPARE(w.infos.size(), 1); // aviso de abertura

    // Mesmo arquivo com outra grafia: exige confirmação; cancelada, nada muda.
    w.legacyAnswer = false;
    w.nextSavePath = dir.filePath("ANTIGO.CADMEC");
    QVERIFY(!w.saveDocumentAs());
    QCOMPARE(w.legacyConfirmations, 1);
    QCOMPARE(w.overwriteConfirmations, 0); // só a confirmação MED, sem a de substituição
    QCOMPARE(readBytes(path), original);
    QCOMPARE(w.documentPath(), path);

    // Outro caminho: sem confirmação; mensagem informativa; original preservado.
    const QString copy = dir.filePath("copia.cadmec");
    w.nextSavePath = copy;
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.legacyConfirmations, 1);
    QCOMPARE(w.infos.size(), 2);
    QVERIFY(w.infos[1].contains("esquema 11") && w.infos[1].contains("preservado"));
    QCOMPARE(readBytes(path), original);
    QCOMPARE(w.documentPath(), copy);
    QCOMPARE(jsonOf(copy)["schemaVersion"].toInt(), 11);
    QVERIFY(!jsonOf(copy)["document"].toObject().contains("demProperties"));

    // Salvar a cópia de novo: nem confirmação nem nova mensagem.
    QVERIFY(w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 1);
    QCOMPARE(w.infos.size(), 2);

    // Salvar como de volta sobre o original (que ainda tem o RFC): exige confirmação.
    w.legacyAnswer = true;
    w.nextSavePath = path;
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.legacyConfirmations, 2);
    QCOMPARE(jsonOf(path)["schemaVersion"].toInt(), 11);
    // Depois de substituído, o original não pede mais confirmação.
    QVERIFY(w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 2);

    // Teste 8 — arquivo de esquema 11 com propriedades: Salvar e Salvar como sem confirmação nem mensagem.
    w.newExampleDocument();
    QVERIFY(w.applyDemProperties(sample()));
    const QString current = dir.filePath("atual.cadmec");
    w.nextSavePath = current;
    QVERIFY(w.saveDocumentAs());
    w.newDocument();
    w.nextOpenPath = current;
    w.openDocument();
    const int infos = w.infos.size();
    QVERIFY(w.saveDocument());
    w.nextSavePath = dir.filePath("atual_copia.cadmec");
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.legacyConfirmations, 2);
    QCOMPARE(w.infos.size(), infos);
    QVERIFY(w.document().demProperties() && *w.document().demProperties() == sample());
    // Esquema 11 sem propriedades MED: também sem confirmação.
    w.newExampleDocument();
    w.nextSavePath = dir.filePath("sem_med.cadmec");
    QVERIFY(w.saveDocumentAs());
    QVERIFY(w.saveDocument());
    QCOMPARE(w.legacyConfirmations, 2);
    QCOMPARE(w.infos.size(), infos);
    QVERIFY(w.errors.isEmpty());
}

// Teste 9 — documento de esquema 10 modificado; ao abrir outro, "Salvar" no pedido de salvar
// alterações e cancelamento da confirmação: a abertura é interrompida e nada muda.
void TestE574aUi::cancelarSalvamentoAoAbrirOutro()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("antigo.cadmec");
    const QByteArray original = schema10File();
    QVERIFY(writeBytes(path, original));
    const QString other = dir.filePath("outro.cadmec");
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    w.newExampleDocument();
    w.nextSavePath = other;
    QVERIFY(w.saveDocumentAs());

    w.nextOpenPath = path;
    w.openDocument();
    QCOMPARE(w.documentPath(), path);
    cad::document::PartParameters changed = w.document().parameters();
    changed.length += 5.0;
    QVERIFY(w.applyParameters(changed));
    QVERIFY(w.document().isModified());

    w.saveChoice = MainWindow::SaveChoice::Save;
    w.legacyAnswer = false;
    w.nextOpenPath = other;
    w.openDocument();
    QCOMPARE(w.legacyConfirmations, 1);
    QCOMPARE(w.documentPath(), path); // abertura interrompida
    QVERIFY(w.document().isModified());
    QCOMPARE(w.document().parameters(), changed);
    QVERIFY(!w.document().demProperties());
    QCOMPARE(readBytes(path), original);

    // Confirmado, salva (esquema 11) e abre o outro documento.
    w.legacyAnswer = true;
    w.openDocument();
    QCOMPARE(w.legacyConfirmations, 2);
    QCOMPARE(w.documentPath(), other);
    QCOMPARE(jsonOf(path)["schemaVersion"].toInt(), 11);
    QVERIFY(w.errors.isEmpty());
}

// Salvar como com grafias equivalentes (esquema 11): o próprio documento — maiúsculas/minúsculas
// diferentes, componentes '.' e '..' — não pede confirmação de substituição; outro arquivo
// existente (inclusive com grafia diferente) sempre pede, e cancelar impede a gravação.
void TestE574aUi::salvarComoGrafiasEquivalentes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir("sub"));
    const QString current = dir.filePath("atual.cadmec");
    const QString other = dir.filePath("outro.cadmec");
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));

    // Outro arquivo existente, com conteúdo distinto.
    w.newExampleDocument();
    w.nextSavePath = other;
    QVERIFY(w.saveDocumentAs());
    const QByteArray otherBytes = readBytes(other);

    // Documento atual (esquema 11, com propriedades MED).
    w.newExampleDocument();
    QVERIFY(w.applyDemProperties(sample()));
    w.nextSavePath = current;
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, 0); // destino novo

    // Próprio documento com grafias equivalentes: sem confirmação, gravado.
    const QString equivalents[] = {dir.filePath("ATUAL.CADMEC"), dir.filePath("Atual.Cadmec"),
                                   dir.path() + "/./atual.cadmec", dir.path() + "/sub/../atual.cadmec",
                                   QDir::toNativeSeparators(dir.path() + "/sub/../ATUAL.cadmec")};
    for (const QString& path : equivalents)
    {
        QVERIFY(w.applyParameters([&] {
            cad::document::PartParameters p = w.document().parameters();
            p.length += 1.0;
            return p;
        }()));
        QVERIFY(w.document().isModified());
        w.nextSavePath = path;
        QVERIFY2(w.saveDocumentAs(), qPrintable(path));
        QVERIFY2(w.overwriteConfirmations == 0, qPrintable(path));
        QVERIFY(!w.document().isModified());
        cad::document::CadDocument reopened;
        QVERIFY(cad::document::DocumentFile::load(current, reopened).ok);
        QCOMPARE(reopened.parameters(), w.document().parameters()); // gravado no próprio arquivo
        QVERIFY(reopened.demProperties() && *reopened.demProperties() == sample());
    }
    QCOMPARE(readBytes(other), otherBytes); // nenhuma grafia atingiu o outro arquivo
    QCOMPARE(w.legacyConfirmations, 0);

    // Outro arquivo existente (várias grafias): pede confirmação; cancelada, nada é gravado e o
    // estado do documento não muda.
    QVERIFY(w.applyParameters([&] {
        cad::document::PartParameters p = w.document().parameters();
        p.width += 1.0;
        return p;
    }()));
    const QString pathBefore = w.documentPath();
    const QByteArray currentBytes = readBytes(current);
    w.overwriteAnswer = false;
    int expected = 0;
    for (const QString& path : {other, dir.filePath("OUTRO.CADMEC"), dir.path() + "/sub/../outro.cadmec"})
    {
        w.nextSavePath = path;
        QVERIFY2(!w.saveDocumentAs(), qPrintable(path));
        QCOMPARE(w.overwriteConfirmations, ++expected);
        QCOMPARE(readBytes(other), otherBytes);
        QCOMPARE(readBytes(current), currentBytes);
        QCOMPARE(w.documentPath(), pathBefore);
        QVERIFY(w.document().isModified());
    }
    // Confirmada: grava o outro arquivo, que passa a ser o documento; o anterior fica intacto.
    w.overwriteAnswer = true;
    w.nextSavePath = dir.filePath("OUTRO.CADMEC");
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, ++expected);
    QVERIFY(readBytes(other) != otherBytes);
    QCOMPARE(readBytes(current), currentBytes);
    QVERIFY(!w.document().isModified());

    // Documento novo, nunca salvo: destino existente sempre pede confirmação.
    w.newExampleDocument();
    w.overwriteAnswer = false;
    w.nextSavePath = current;
    QVERIFY(!w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, ++expected);
    QCOMPARE(readBytes(current), currentBytes);

    // Aberto por grafia diferente: Salvar e Salvar como no próprio arquivo sem confirmação.
    w.newDocument();
    w.nextOpenPath = dir.filePath("ATUAL.CADMEC");
    w.openDocument();
    QVERIFY(w.document().demProperties());
    QVERIFY(w.saveDocument());
    w.nextSavePath = current;
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, expected);
    QCOMPARE(w.legacyConfirmations, 0);
    QCOMPARE(w.infos.size(), 0);
    QVERIFY(w.errors.isEmpty());
}

// Salvar como com grafias equivalentes (esquema 10): sobre o arquivo antigo, só a confirmação MED
// (sem a de substituição), e cancelar não grava; sobre outro arquivo existente, só a de
// substituição, e o original é preservado.
void TestE574aUi::salvarComoGrafiasEquivalentesEsquema10()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir("sub"));
    const QString path = dir.filePath("antigo.cadmec");
    const QByteArray original = schema10File();
    QVERIFY(writeBytes(path, original));
    const QString other = dir.filePath("outro.cadmec");
    TestableMainWindow w;
    QVERIFY(showMainWindow(w));
    w.newExampleDocument();
    w.nextSavePath = other;
    QVERIFY(w.saveDocumentAs());
    const QByteArray otherBytes = readBytes(other);

    w.nextOpenPath = path;
    w.openDocument();
    QCOMPARE(w.documentPath(), path);
    w.legacyAnswer = false;
    int legacy = 0;
    for (const QString& same : {dir.filePath("ANTIGO.CADMEC"), dir.path() + "/./antigo.cadmec",
                                dir.path() + "/sub/../Antigo.cadmec"})
    {
        w.nextSavePath = same;
        QVERIFY2(!w.saveDocumentAs(), qPrintable(same));
        QCOMPARE(w.legacyConfirmations, ++legacy);
        QCOMPARE(w.overwriteConfirmations, 0);
        QCOMPARE(readBytes(path), original);
        QCOMPARE(w.documentPath(), path);
    }

    // Outro arquivo existente: só a confirmação de substituição; cancelada, nada é gravado.
    w.overwriteAnswer = false;
    w.nextSavePath = dir.filePath("OUTRO.cadmec");
    QVERIFY(!w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, 1);
    QCOMPARE(w.legacyConfirmations, legacy);
    QCOMPARE(readBytes(other), otherBytes);
    QCOMPARE(readBytes(path), original);
    // Confirmada: cópia gravada no esquema 11 sem propriedades MED, com mensagem; original preservado.
    w.overwriteAnswer = true;
    const int infos = w.infos.size();
    QVERIFY(w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, 2);
    QCOMPARE(w.legacyConfirmations, legacy);
    QCOMPARE(w.infos.size(), infos + 1);
    QCOMPARE(readBytes(path), original);
    QCOMPARE(jsonOf(other)["schemaVersion"].toInt(), 11);
    QVERIFY(!jsonOf(other)["document"].toObject().contains("demProperties"));

    // De volta sobre o original por outra grafia: é outro arquivo em relação à cópia aberta, então
    // pede substituição e também a confirmação MED; cancelar a MED não grava.
    w.legacyAnswer = false;
    w.nextSavePath = dir.path() + "/sub/../ANTIGO.cadmec";
    QVERIFY(!w.saveDocumentAs());
    QCOMPARE(w.overwriteConfirmations, 3);
    QCOMPARE(w.legacyConfirmations, legacy + 1);
    QCOMPARE(readBytes(path), original);
    QVERIFY(w.errors.isEmpty());
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
