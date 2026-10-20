// Formato nativo .cadmec (E5.1): salvar, abrir, validar e rejeitar arquivos inválidos.
#include "document/DocumentFile.h"
#include "modeling/ShapeInspector.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace cad::document;
using cad::modeling::ShapeInspector;

namespace {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// JSON válido de referência com uma alteração aplicada por 'edit'.
template <typename F>
QByteArray modified(F edit)
{
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(CadDocument())).object();
    edit(root);
    return QJsonDocument(root).toJson();
}

QJsonObject op(const QJsonObject& root, int i)
{
    return root["document"].toObject()["operations"].toArray()[i].toObject();
}

void setOp(QJsonObject& root, int i, const QJsonObject& value)
{
    QJsonObject doc = root["document"].toObject();
    QJsonArray ops = doc["operations"].toArray();
    ops[i] = value;
    doc["operations"] = ops;
    root["document"] = doc;
}

void setParam(QJsonObject& root, int i, const char* key, const QJsonValue& value)
{
    QJsonObject o = op(root, i);
    QJsonObject params = o["parameters"].toObject();
    params[key] = value;
    o["parameters"] = params;
    setOp(root, i, o);
}

} // namespace

class TestArquivoCadmec : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void salvarDocumento();
    void abrirDocumento();
    void idaEVoltaPreservaTudo();
    void salvamentoSeguroSemTemporarios();
    void salvarEmDiretorioInexistente();
    void rejeitaArquivoCorrompido_data();
    void rejeitaArquivoCorrompido();
    void preservaDocumentoAtivoAposFalha();
    void rejeitaArquivoInexistenteEGrande();

private:
    QTemporaryDir* m_dir = nullptr;
};

void TestArquivoCadmec::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
}

void TestArquivoCadmec::cleanup()
{
    delete m_dir;
    m_dir = nullptr;
}

void TestArquivoCadmec::salvarDocumento()
{
    CadDocument doc("Flange de teste");
    QVERIFY(doc.setParameters({120.0, 70.0, 15.0, 12.5}).ok);
    const QString path = m_dir->filePath("flange.cadmec");
    const FileResult r = DocumentFile::save(doc, path);
    QVERIFY2(r.ok, qPrintable(r.message));
    QVERIFY(QFile::exists(path));

    // Conteúdo: esquema versionado, parâmetros e sequência de operações (não geometria).
    const QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["format"].toString(), QStringLiteral("cadmec"));
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    const QJsonObject d = root["document"].toObject();
    QCOMPARE(d["name"].toString(), QStringLiteral("Flange de teste"));
    QCOMPARE(d["unit"].toString(), QStringLiteral("mm"));
    const QJsonArray ops = d["operations"].toArray();
    QCOMPARE(ops.size(), 2);
    QCOMPARE(ops[0].toObject()["id"].toString(), QStringLiteral("op-1"));
    QCOMPARE(ops[0].toObject()["type"].toString(), QStringLiteral("block"));
    QCOMPARE(ops[0].toObject()["parameters"].toObject()["length"].toDouble(), 120.0);
    QCOMPARE(ops[1].toObject()["type"].toString(), QStringLiteral("throughHole"));
    QCOMPARE(ops[1].toObject()["parameters"].toObject()["radius"].toDouble(), 12.5);
    QVERIFY(!readFile(path).contains("ISO-10303")); // não é um STEP/representação gráfica
}

void TestArquivoCadmec::abrirDocumento()
{
    const QString path = m_dir->filePath("peca.cadmec");
    writeFile(path, modified([](QJsonObject& root) {
        QJsonObject d = root["document"].toObject();
        d["name"] = "Suporte";
        root["document"] = d;
        setParam(root, 0, "length", 90.0);
        setParam(root, 1, "radius", 7.0);
    }));
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(QString::fromStdString(doc.name()), QStringLiteral("Suporte"));
    QCOMPARE(doc.parameters(), (PartParameters{90.0, 60.0, 20.0, 7.0}));
    QVERIFY(!doc.isModified());
    // Geometria reconstruída a partir dos parâmetros.
    const auto s = ShapeInspector::inspect(doc.shape());
    QVERIFY(s.isValid);
    QCOMPARE(s.faces, 7);
    const double expected = 90.0 * 60.0 * 20.0 - 3.14159265358979323846 * 49.0 * 20.0;
    QVERIFY(qAbs(s.volume - expected) < expected * 1e-9);
}

void TestArquivoCadmec::idaEVoltaPreservaTudo()
{
    CadDocument a("Ida e volta");
    QVERIFY(a.setParameters({77.7, 44.4, 33.3, 11.1}).ok);
    const QString path = m_dir->filePath("ida_volta.cadmec");
    QVERIFY(DocumentFile::save(a, path).ok);
    CadDocument b;
    QVERIFY(DocumentFile::load(path, b).ok);
    QCOMPARE(b.name(), a.name());
    QCOMPARE(b.parameters(), a.parameters());
    QCOMPARE(b.operations().size(), a.operations().size());
    for (size_t i = 0; i < a.operations().size(); ++i)
    {
        QCOMPARE(b.operations()[i].id, a.operations()[i].id);
        QVERIFY(b.operations()[i].type == a.operations()[i].type);
    }
    QCOMPARE(ShapeInspector::volume(b.shape()), ShapeInspector::volume(a.shape()));
    // Salvar de novo produz o mesmo conteúdo (determinístico).
    QCOMPARE(DocumentFile::toJson(b), DocumentFile::toJson(a));
}

void TestArquivoCadmec::salvamentoSeguroSemTemporarios()
{
    const QString path = m_dir->filePath("seguro.cadmec");
    QVERIFY(DocumentFile::save(CadDocument("v1"), path).ok);
    QVERIFY(DocumentFile::save(CadDocument("v2"), path).ok); // substituição controlada
    CadDocument doc;
    QVERIFY(DocumentFile::load(path, doc).ok);
    QCOMPARE(QString::fromStdString(doc.name()), QStringLiteral("v2"));
    // Nenhum arquivo temporário deixado no diretório.
    QCOMPARE(QDir(m_dir->path()).entryList(QDir::Files), QStringList{"seguro.cadmec"});
}

void TestArquivoCadmec::salvarEmDiretorioInexistente()
{
    const QString path = m_dir->filePath("nao_existe/doc.cadmec");
    const FileResult r = DocumentFile::save(CadDocument(), path);
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    QVERIFY(!QFile::exists(path));
}

void TestArquivoCadmec::rejeitaArquivoCorrompido_data()
{
    QTest::addColumn<QByteArray>("content");

    QTest::newRow("vazio") << QByteArray();
    QTest::newRow("json truncado") << DocumentFile::toJson(CadDocument()).left(60);
    QTest::newRow("binario") << QByteArray("\x00\x01\x02\xff\xfe", 5);
    QTest::newRow("raiz array") << QByteArray("[1,2,3]");
    QTest::newRow("formato errado") << modified([](QJsonObject& r) { r["format"] = "outro"; });
    QTest::newRow("sem versao") << modified([](QJsonObject& r) { r.remove("schemaVersion"); });
    QTest::newRow("versao texto") << modified([](QJsonObject& r) { r["schemaVersion"] = "1"; });
    QTest::newRow("versao futura") << modified([](QJsonObject& r) { r["schemaVersion"] = 99; });
    QTest::newRow("versao fracionaria") << modified([](QJsonObject& r) { r["schemaVersion"] = 1.5; });
    QTest::newRow("sem documento") << modified([](QJsonObject& r) { r.remove("document"); });
    QTest::newRow("nome vazio") << modified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d["name"] = "  "; r["document"] = d; });
    QTest::newRow("nome numerico") << modified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d["name"] = 5; r["document"] = d; });
    QTest::newRow("unidade polegada") << modified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d["unit"] = "in"; r["document"] = d; });
    QTest::newRow("operacoes ausentes") << modified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d.remove("operations"); r["document"] = d; });
    QTest::newRow("uma operacao") << modified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); QJsonArray a = d["operations"].toArray(); a.removeLast();
        d["operations"] = a; r["document"] = d; });
    QTest::newRow("ordem trocada") << modified([](QJsonObject& r) {
        const QJsonObject a = op(r, 0), b = op(r, 1); setOp(r, 0, b); setOp(r, 1, a); });
    QTest::newRow("tipo desconhecido") << modified([](QJsonObject& r) {
        QJsonObject o = op(r, 1); o["type"] = "chamfer"; setOp(r, 1, o); });
    QTest::newRow("id duplicado") << modified([](QJsonObject& r) {
        QJsonObject o = op(r, 1); o["id"] = "op-1"; setOp(r, 1, o); });
    QTest::newRow("id invalido") << modified([](QJsonObject& r) {
        QJsonObject o = op(r, 0); o["id"] = "op 1/../x"; setOp(r, 0, o); });
    QTest::newRow("id numerico") << modified([](QJsonObject& r) {
        QJsonObject o = op(r, 0); o["id"] = 1; setOp(r, 0, o); });
    QTest::newRow("parametro texto") << modified([](QJsonObject& r) { setParam(r, 0, "length", "100"); });
    QTest::newRow("parametro ausente") << modified([](QJsonObject& r) {
        QJsonObject o = op(r, 0); QJsonObject p = o["parameters"].toObject(); p.remove("height");
        o["parameters"] = p; setOp(r, 0, o); });
    QTest::newRow("parametro desconhecido") << modified([](QJsonObject& r) { setParam(r, 1, "depth", 5.0); });
    QTest::newRow("comprimento negativo") << modified([](QJsonObject& r) { setParam(r, 0, "length", -100.0); });
    QTest::newRow("altura zero") << modified([](QJsonObject& r) { setParam(r, 0, "height", 0.0); });
    QTest::newRow("dimensao enorme") << modified([](QJsonObject& r) { setParam(r, 0, "width", 1e9); });
    QTest::newRow("furo maior que o bloco") << modified([](QJsonObject& r) { setParam(r, 1, "radius", 40.0); });
    QTest::newRow("furo nulo") << modified([](QJsonObject& r) { setParam(r, 1, "radius", 0.0); });
}

void TestArquivoCadmec::rejeitaArquivoCorrompido()
{
    QFETCH(QByteArray, content);
    const QString path = m_dir->filePath("corrompido.cadmec");
    writeFile(path, content);

    CadDocument active("Documento ativo");
    QVERIFY(active.setParameters({111.0, 55.0, 22.0, 9.0}).ok);
    const auto paramsBefore = active.parameters();
    const TopoDS_Shape shapeBefore = active.shape();

    const FileResult r = DocumentFile::load(path, active);
    qInfo("%s -> %s", QTest::currentDataTag(), qPrintable(r.message));
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    // Documento ativo intacto.
    QCOMPARE(QString::fromStdString(active.name()), QStringLiteral("Documento ativo"));
    QCOMPARE(active.parameters(), paramsBefore);
    QVERIFY(active.shape().IsSame(shapeBefore));
    QVERIFY(active.isModified());
}

void TestArquivoCadmec::preservaDocumentoAtivoAposFalha()
{
    // Arquivo válido -> carregado; em seguida arquivo inválido não altera o documento carregado.
    const QString good = m_dir->filePath("bom.cadmec");
    CadDocument saved("Bom");
    QVERIFY(saved.setParameters({60.0, 50.0, 40.0, 20.0}).ok);
    QVERIFY(DocumentFile::save(saved, good).ok);

    CadDocument active;
    QVERIFY(DocumentFile::load(good, active).ok);
    const QString bad = m_dir->filePath("ruim.cadmec");
    writeFile(bad, modified([](QJsonObject& r) { setParam(r, 1, "radius", 1000.0); }));
    QVERIFY(!DocumentFile::load(bad, active).ok);
    QCOMPARE(QString::fromStdString(active.name()), QStringLiteral("Bom"));
    QCOMPARE(active.parameters(), (PartParameters{60.0, 50.0, 40.0, 20.0}));
    QVERIFY(!active.isModified());
}

void TestArquivoCadmec::rejeitaArquivoInexistenteEGrande()
{
    CadDocument active("ativo");
    QVERIFY(!DocumentFile::load(m_dir->filePath("nao_existe.cadmec"), active).ok);

    const QString big = m_dir->filePath("grande.cadmec");
    writeFile(big, QByteArray(DocumentFile::kMaxFileSize + 1, ' '));
    const FileResult r = DocumentFile::load(big, active);
    QVERIFY(!r.ok);
    QCOMPARE(QString::fromStdString(active.name()), QStringLiteral("ativo"));
}

QTEST_GUILESS_MAIN(TestArquivoCadmec)
#include "test_arquivo_cadmec.moc"
