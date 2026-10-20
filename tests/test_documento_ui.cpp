// Interface do documento paramétrico (E5.1): árvore, edição de parâmetros, novo/salvar/abrir,
// exportação STEP após edição e fechamento. Diálogos modais substituídos por respostas programadas.
#include "app/MainWindow.h"
#include "app/ParametersDialog.h"
#include "document/DocumentFile.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <filesystem>
#include <optional>

using cad::app::MainWindow;
using cad::document::CadDocument;
using cad::document::DocumentFile;
using cad::document::PartParameters;

namespace {

constexpr double kPi = 3.14159265358979323846;

double volumeOf(const PartParameters& p)
{
    return p.length * p.width * p.height - kPi * p.holeRadius * p.holeRadius * p.height;
}

class TestableMainWindow : public MainWindow
{
public:
    std::optional<PartParameters> nextParameters;
    QString nextOpenPath;
    QString nextSavePath;
    QString nextExportPath;
    SaveChoice saveChoice = SaveChoice::Cancel;
    bool overwriteAnswer = false;
    int parameterRequests = 0;
    int saveChangesRequests = 0;
    int overwriteRequests = 0;
    QStringList infos;
    QStringList errors;

protected:
    std::optional<PartParameters> askParameters(const PartParameters&) override
    {
        ++parameterRequests;
        return nextParameters;
    }
    QString askOpenPath(const QString&) override { return nextOpenPath; }
    QString askSavePath(const QString&) override { return nextSavePath; }
    QString askExportPath(const QString&) override { return nextExportPath; }
    SaveChoice askSaveChanges() override
    {
        ++saveChangesRequests;
        return saveChoice;
    }
    bool confirmOverwrite(const QString&) override
    {
        ++overwriteRequests;
        return overwriteAnswer;
    }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
    void reportError(const QString&, const QString& text) override { errors << text; }
};

QTreeWidgetItem* operationItem(QTreeWidget* tree, const QString& id)
{
    QTreeWidgetItem* root = tree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
        if (root->child(i)->data(0, MainWindow::kItemIdRole).toString() == id)
            return root->child(i);
    return nullptr;
}

QStringList childTexts(QTreeWidgetItem* item)
{
    QStringList texts;
    for (int i = 0; item && i < item->childCount(); ++i)
        texts << item->child(i)->text(0);
    return texts;
}

} // namespace

class TestDocumentoUi : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void arvoreConectadaAoDocumento();
    void selecaoDeOperacaoMostraPropriedades();
    void editarParametrosAtualizaTudo();
    void editarCanceladoNaoAltera();
    void parametrosInvalidosRecusados();
    void dialogoParametrosReal();
    void dialogoParametrosCancelar();
    void novoDocumento();
    void salvarEAbrir();
    void salvarNaoSobrescreveSemConfirmacao();
    void abrirCorrompidoPreservaAtivo();
    void exportarStepAposEdicao();
    void fecharSemAlteracoes();
    void fecharComAlteracoes();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }
    QTreeWidget* tree() const { return m_window->operationsTree(); }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestDocumentoUi::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
    m_window = new TestableMainWindow;
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
}

void TestDocumentoUi::cleanup()
{
    if (m_window)
        m_window->saveChoice = MainWindow::SaveChoice::Discard;
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestDocumentoUi::arvoreConectadaAoDocumento()
{
    QCOMPARE(tree()->topLevelItemCount(), 1);
    QTreeWidgetItem* root = tree()->topLevelItem(0);
    QCOMPARE(root->text(0), QStringLiteral("Peça de teste"));
    QCOMPARE(root->data(0, MainWindow::kItemKindRole).toString(), QStringLiteral("documento"));
    QCOMPARE(root->childCount(), 2);

    QTreeWidgetItem* block = operationItem(tree(), "op-1");
    QTreeWidgetItem* hole = operationItem(tree(), "op-2");
    QVERIFY(block && hole);
    QCOMPARE(block->text(0), QStringLiteral("Bloco base"));
    QCOMPARE(hole->text(0), QStringLiteral("Furo passante"));
    QCOMPARE(childTexts(block), QStringList({"Comprimento: 100,00 mm", "Largura: 60,00 mm", "Altura: 20,00 mm",
                                             "Estado: Válida"}));
    QCOMPARE(childTexts(hole), QStringList({"Raio: 10,00 mm", "Estado: Válida"}));
}

void TestDocumentoUi::selecaoDeOperacaoMostraPropriedades()
{
    auto* panel = m_window->findChild<QLabel*>("painelPropriedades");
    QVERIFY(panel);
    tree()->setCurrentItem(operationItem(tree(), "op-2"));
    QVERIFY(panel->text().contains("Furo passante"));
    QVERIFY(panel->text().contains("op-2"));
    QVERIFY(panel->text().contains("Diâmetro: 20,00 mm"));
    QVERIFY(panel->text().contains("Válida"));

    tree()->setCurrentItem(operationItem(tree(), "op-1")->child(0)); // parâmetro do bloco
    QVERIFY(panel->text().contains("Bloco base"));
    QVERIFY(panel->text().contains("Comprimento: 100,00 mm"));

    tree()->setCurrentItem(tree()->topLevelItem(0));
    QVERIFY(panel->text().contains("Peça de teste"));
    QVERIFY(panel->text().contains("Unidade: mm"));
    // Seleção na árvore não seleciona entidades do visualizador.
    QVERIFY(m_window->viewer()->selectedShapes().isEmpty());
}

void TestDocumentoUi::editarParametrosAtualizaTudo()
{
    tree()->setCurrentItem(operationItem(tree(), "op-2"));
    const PartParameters p{150.0, 80.0, 25.0, 12.0};
    m_window->nextParameters = p;
    action("actionEditarParametros")->trigger();

    QCOMPARE(m_window->parameterRequests, 1);
    QVERIFY(m_window->errors.isEmpty());
    // Documento
    QCOMPARE(m_window->document().parameters(), p);
    QVERIFY(m_window->document().isModified());
    QCOMPARE(m_window->document().revision(), 2);
    QVERIFY(qAbs(cad::modeling::ShapeInspector::volume(m_window->part()) - volumeOf(p)) < 1e-6);
    // Visualizador recebe a nova geometria
    QVERIFY(m_window->viewer()->shape().IsSame(m_window->part()));
    // Árvore e título
    QCOMPARE(childTexts(operationItem(tree(), "op-1")),
             QStringList({"Comprimento: 150,00 mm", "Largura: 80,00 mm", "Altura: 25,00 mm", "Estado: Válida"}));
    QCOMPARE(childTexts(operationItem(tree(), "op-2")), QStringList({"Raio: 12,00 mm", "Estado: Válida"}));
    QCOMPARE(tree()->topLevelItem(0)->text(0), QStringLiteral("Peça de teste *"));
    QVERIFY(m_window->isWindowModified());
    // A operação selecionada continua selecionada após a atualização.
    QCOMPARE(tree()->currentItem()->data(0, MainWindow::kItemIdRole).toString(), QStringLiteral("op-2"));
    QVERIFY(m_window->statusBar()->currentMessage().contains("recalculada"));
}

void TestDocumentoUi::editarCanceladoNaoAltera()
{
    const int revision = m_window->document().revision();
    m_window->nextParameters.reset(); // cancelar
    action("actionEditarParametros")->trigger();
    QCOMPARE(m_window->parameterRequests, 1);
    QCOMPARE(m_window->document().parameters(), PartParameters());
    QCOMPARE(m_window->document().revision(), revision);
    QVERIFY(!m_window->document().isModified());
    QVERIFY(!m_window->isWindowModified());

    // Duplo clique em uma operação abre a edição.
    QTreeWidgetItem* block = operationItem(tree(), "op-1");
    emit tree()->itemDoubleClicked(block, 0);
    QCOMPARE(m_window->parameterRequests, 2);
}

void TestDocumentoUi::parametrosInvalidosRecusados()
{
    const TopoDS_Shape before = m_window->part();
    QVERIFY(!m_window->applyParameters({100.0, 60.0, 20.0, 35.0}));
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains("Raio do furo"));
    QVERIFY(m_window->errors.first().contains("último estado válido"));
    QVERIFY(m_window->part().IsSame(before));
    QVERIFY(m_window->viewer()->shape().IsSame(before));
    QCOMPARE(m_window->document().parameters(), PartParameters());
    QVERIFY(!m_window->document().isModified());
    QCOMPARE(childTexts(operationItem(tree(), "op-2")).first(), QStringLiteral("Raio: 10,00 mm"));
    QVERIFY(m_window->statusBar()->currentMessage().contains("recusados"));

    QVERIFY(!m_window->applyParameters({0.0, 60.0, 20.0, 10.0}));
    QCOMPARE(m_window->errors.size(), 2);
    QVERIFY(m_window->errors.last().contains("Comprimento"));
}

void TestDocumentoUi::dialogoParametrosReal()
{
    cad::app::ParametersDialog dialog(PartParameters{});
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* length = dialog.findChild<QDoubleSpinBox*>("campoComprimento");
    auto* radius = dialog.findChild<QDoubleSpinBox*>("campoRaioFuro");
    auto* buttons = dialog.findChild<QDialogButtonBox*>("botoesParametros");
    QVERIFY(length && radius && buttons);
    QCOMPARE(length->suffix(), QStringLiteral(" mm"));
    QCOMPARE(length->value(), 100.0);
    QCOMPARE(radius->value(), 10.0);

    // Valor inválido digitado: Aplicar não fecha e mostra a mensagem.
    radius->setValue(45.0);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(dialog.validationMessage().contains("Raio do furo"));

    // Valor válido: fecha com os novos parâmetros.
    radius->setValue(14.0);
    length->setValue(130.0);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QVERIFY(!dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.parameters(), (PartParameters{130.0, 60.0, 20.0, 14.0}));
}

void TestDocumentoUi::dialogoParametrosCancelar()
{
    cad::app::ParametersDialog dialog(PartParameters{});
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.findChild<QDoubleSpinBox*>("campoAltura")->setValue(50.0);
    QTest::mouseClick(dialog.findChild<QDialogButtonBox*>("botoesParametros")->button(QDialogButtonBox::Cancel),
                      Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

void TestDocumentoUi::novoDocumento()
{
    QVERIFY(m_window->applyParameters({90.0, 50.0, 10.0, 5.0}));
    // Alterações pendentes + cancelar: nada muda.
    m_window->saveChoice = MainWindow::SaveChoice::Cancel;
    action("actionNovo")->trigger();
    QCOMPARE(m_window->saveChangesRequests, 1);
    QCOMPARE(m_window->document().parameters(), (PartParameters{90.0, 50.0, 10.0, 5.0}));
    // Descartar: novo documento com a peça padrão.
    m_window->saveChoice = MainWindow::SaveChoice::Discard;
    action("actionNovo")->trigger();
    QCOMPARE(m_window->saveChangesRequests, 2);
    QCOMPARE(QString::fromStdString(m_window->document().name()), QStringLiteral("Nova peça"));
    QCOMPARE(m_window->document().parameters(), PartParameters());
    QVERIFY(!m_window->document().isModified());
    QVERIFY(m_window->documentPath().isEmpty());
    QCOMPARE(tree()->topLevelItem(0)->text(0), QStringLiteral("Nova peça"));
    QVERIFY(m_window->viewer()->shape().IsSame(m_window->part()));
}

void TestDocumentoUi::salvarEAbrir()
{
    const PartParameters p{140.0, 70.0, 30.0, 18.0};
    QVERIFY(m_window->applyParameters(p));

    // Salvar sem caminho -> Salvar como; extensão acrescentada.
    m_window->nextSavePath = m_dir->filePath("suporte");
    action("actionSalvar")->trigger();
    const QString path = m_dir->filePath("suporte.cadmec");
    QVERIFY(QFile::exists(path));
    QCOMPARE(m_window->documentPath(), QFileInfo(path).absoluteFilePath());
    QVERIFY(!m_window->document().isModified());
    QVERIFY(!m_window->isWindowModified());
    QVERIFY(m_window->errors.isEmpty());

    // Novo documento e depois abrir o arquivo salvo.
    action("actionNovo")->trigger();
    QCOMPARE(m_window->document().parameters(), PartParameters());
    m_window->nextOpenPath = path;
    action("actionAbrir")->trigger();
    QVERIFY(m_window->errors.isEmpty());
    QCOMPARE(m_window->document().parameters(), p);
    QCOMPARE(m_window->documentPath(), QFileInfo(path).absoluteFilePath());
    QCOMPARE(childTexts(operationItem(tree(), "op-1")).first(), QStringLiteral("Comprimento: 140,00 mm"));
    QVERIFY(m_window->viewer()->shape().IsSame(m_window->part()));
    QVERIFY(qAbs(cad::modeling::ShapeInspector::volume(m_window->part()) - volumeOf(p)) < 1e-6);

    // Edição + Salvar grava no mesmo arquivo sem pedir caminho nem confirmação.
    QVERIFY(m_window->applyParameters({140.0, 70.0, 30.0, 20.0}));
    m_window->nextSavePath.clear();
    action("actionSalvar")->trigger();
    QCOMPARE(m_window->overwriteRequests, 0);
    CadDocument reread;
    QVERIFY(DocumentFile::load(path, reread).ok);
    QCOMPARE(reread.parameters().holeRadius, 20.0);
}

void TestDocumentoUi::salvarNaoSobrescreveSemConfirmacao()
{
    const QString path = m_dir->filePath("existente.cadmec");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("conteudo original");
    }
    m_window->nextSavePath = path;
    m_window->overwriteAnswer = false;
    action("actionSalvarComo")->trigger();
    QCOMPARE(m_window->overwriteRequests, 1);
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("conteudo original"));
    f.close();
    QVERIFY(m_window->documentPath().isEmpty());

    m_window->overwriteAnswer = true;
    action("actionSalvarComo")->trigger();
    QCOMPARE(m_window->overwriteRequests, 2);
    CadDocument reread;
    QVERIFY(DocumentFile::load(path, reread).ok);
}

void TestDocumentoUi::abrirCorrompidoPreservaAtivo()
{
    const PartParameters p{120.0, 65.0, 22.0, 11.0};
    QVERIFY(m_window->applyParameters(p));
    const TopoDS_Shape before = m_window->part();

    const QString bad = m_dir->filePath("corrompido.cadmec");
    {
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ \"format\": \"cadmec\", \"schemaVersion\": 1, \"document\": { \"name\": ");
    }
    m_window->saveChoice = MainWindow::SaveChoice::Discard;
    m_window->nextOpenPath = bad;
    action("actionAbrir")->trigger();
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->errors.first().contains("documento atual foi mantido"));
    // Documento ativo intacto (inclusive alterações não salvas).
    QCOMPARE(m_window->document().parameters(), p);
    QVERIFY(m_window->document().isModified());
    QVERIFY(m_window->part().IsSame(before));
    QVERIFY(m_window->viewer()->shape().IsSame(before));
    QCOMPARE(childTexts(operationItem(tree(), "op-1")).first(), QStringLiteral("Comprimento: 120,00 mm"));
    QVERIFY(m_window->documentPath().isEmpty());

    QVERIFY(!m_window->openDocumentFrom(m_dir->filePath("nao_existe.cadmec")));
    QCOMPARE(m_window->document().parameters(), p);
}

void TestDocumentoUi::exportarStepAposEdicao()
{
    const PartParameters p{160.0, 90.0, 12.0, 25.0};
    QVERIFY(m_window->applyParameters(p));
    m_window->nextExportPath = m_dir->filePath("editada.step");
    action("actionExportarStep")->trigger();
    QCOMPARE(m_window->infos.size(), 1);
    const auto read = cad::io::StepImporter().read(std::filesystem::path(m_window->nextExportPath.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    const auto s = cad::modeling::ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 7);
    QVERIFY(qAbs(s.volume - volumeOf(p)) < volumeOf(p) * 1e-9);
}

void TestDocumentoUi::fecharSemAlteracoes()
{
    m_window->close();
    QVERIFY(!m_window->isVisible());
    QCOMPARE(m_window->saveChangesRequests, 0);
}

void TestDocumentoUi::fecharComAlteracoes()
{
    QVERIFY(m_window->applyParameters({101.0, 60.0, 20.0, 10.0}));
    // Cancelar: a janela continua aberta.
    m_window->saveChoice = MainWindow::SaveChoice::Cancel;
    QVERIFY(!m_window->close());
    QVERIFY(m_window->isVisible());
    // Salvar: grava e fecha.
    m_window->saveChoice = MainWindow::SaveChoice::Save;
    m_window->nextSavePath = m_dir->filePath("ao_fechar.cadmec");
    QVERIFY(m_window->close());
    QVERIFY(!m_window->isVisible());
    QVERIFY(QFile::exists(m_dir->filePath("ao_fechar.cadmec")));
    QCOMPARE(m_window->saveChangesRequests, 2);
}

QTEST_MAIN(TestDocumentoUi)
#include "test_documento_ui.moc"
