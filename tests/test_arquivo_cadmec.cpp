// Formato nativo .cadmec (E5.1): salvar, abrir, validar e rejeitar arquivos inválidos.
#include "document/DocumentFile.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchShapes.h"

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
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(CadDocument::withTestPart())).object();
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
    // E5.2
    void salvarEsbocos();
    void abrirEReconstruirEsbocos();
    void compatibilidadeArquivoE51();
    void rejeitaEsbocoInvalido_data();
    void rejeitaEsbocoInvalido();
    // E5.2.1
    void documentoVazio();
    void documentoSomenteComEsbocos();
    void arquivoE52ComBlocoEFuro();
    void operacoesVaziasPorVersao();
    // E5.3
    void salvarEAbrirExtrusao();
    void arquivoE521SomenteEsbocos();
    void rejeitaExtrusaoInvalida_data();
    void rejeitaExtrusaoInvalida();

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
    CadDocument doc = CadDocument::withTestPart("Flange de teste");
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
    CadDocument a = CadDocument::withTestPart("Ida e volta");
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
    QTest::newRow("json truncado") << DocumentFile::toJson(CadDocument::withTestPart()).left(60);
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

    CadDocument active = CadDocument::withTestPart("Documento ativo");
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
    CadDocument saved = CadDocument::withTestPart("Bom");
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

namespace {

// Documento com três esboços (um por plano) e entidades de todos os tipos.
CadDocument documentWithSketches()
{
    using namespace cad::sketch;
    CadDocument doc = CadDocument::withTestPart("Com esboços");
    std::string error;
    Sketch a = doc.createSketch(PlaneType::XY);
    a.addEntity(SketchLine{0.0, 0.0, 50.0, 0.0});
    a.addEntity(SketchRectangle{10.0, 10.0, 40.0, 20.0});
    a.addEntity(SketchCircle{50.0, 30.0, 8.0});
    std::string removed;
    a.addEntity(SketchLine{1.0, 1.0, 2.0, 2.0}, &removed);
    a.removeEntity(removed); // contador 5, ids e-1..e-3
    doc.commitSketch(a, error);
    Sketch b = doc.createSketch(PlaneType::XZ);
    b.addEntity(SketchCircle{-12.5, 7.25, 3.5});
    doc.commitSketch(b, error);
    Sketch c = doc.createSketch(PlaneType::YZ);
    doc.commitSketch(c, error); // esboço vazio
    return doc;
}

QJsonObject sketchObject(QJsonObject& root, int i)
{
    return root["document"].toObject()["sketches"].toArray()[i].toObject();
}

void setSketch(QJsonObject& root, int i, const QJsonObject& value)
{
    QJsonObject doc = root["document"].toObject();
    QJsonArray sketches = doc["sketches"].toArray();
    sketches[i] = value;
    doc["sketches"] = sketches;
    root["document"] = doc;
}

void setEntityField(QJsonObject& root, int sketch, int entity, const char* key, const QJsonValue& value)
{
    QJsonObject s = sketchObject(root, sketch);
    QJsonArray entities = s["entities"].toArray();
    QJsonObject e = entities[entity].toObject();
    if (value.isUndefined())
        e.remove(key);
    else
        e[key] = value;
    entities[entity] = e;
    s["entities"] = entities;
    setSketch(root, sketch, s);
}

template <typename F>
QByteArray sketchesModified(F edit)
{
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(documentWithSketches())).object();
    edit(root);
    return QJsonDocument(root).toJson();
}

} // namespace

void TestArquivoCadmec::salvarEsbocos()
{
    const QString path = m_dir->filePath("esbocos.cadmec");
    QVERIFY(DocumentFile::save(documentWithSketches(), path).ok);
    const QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    const QJsonObject d = root["document"].toObject();
    QCOMPARE(d["nextSketchNumber"].toInt(), 4);
    const QJsonArray sketches = d["sketches"].toArray();
    QCOMPARE(sketches.size(), 3);
    const QJsonObject a = sketches[0].toObject();
    QCOMPARE(a["id"].toString(), QStringLiteral("sk-1"));
    QCOMPARE(a["name"].toString(), QStringLiteral("Esboço 1"));
    QCOMPARE(a["plane"].toString(), QStringLiteral("XY"));
    QCOMPARE(a["nextEntityNumber"].toInt(), 5);
    const QJsonArray e = a["entities"].toArray();
    QCOMPARE(e.size(), 3);
    QCOMPARE(e[0].toObject()["type"].toString(), QStringLiteral("line"));
    QCOMPARE(e[0].toObject()["x2"].toDouble(), 50.0);
    QCOMPARE(e[1].toObject()["type"].toString(), QStringLiteral("rectangle"));
    QCOMPARE(e[1].toObject()["height"].toDouble(), 20.0);
    QCOMPARE(e[2].toObject()["type"].toString(), QStringLiteral("circle"));
    QCOMPARE(e[2].toObject()["id"].toString(), QStringLiteral("e-3"));
    QCOMPARE(sketches[1].toObject()["plane"].toString(), QStringLiteral("XZ"));
    QCOMPARE(sketches[2].toObject()["entities"].toArray().size(), 0);
    // Campos da versão 1 mantêm o significado.
    QCOMPARE(d["operations"].toArray()[0].toObject()["parameters"].toObject()["length"].toDouble(), 100.0);
}

void TestArquivoCadmec::abrirEReconstruirEsbocos()
{
    using namespace cad::sketch;
    const CadDocument original = documentWithSketches();
    const QString path = m_dir->filePath("reabrir.cadmec");
    QVERIFY(DocumentFile::save(original, path).ok);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.sketches().size(), size_t(3));
    QCOMPARE(doc.nextSketchNumber(), 4);
    for (size_t i = 0; i < 3; ++i)
    {
        const Sketch& a = original.sketches()[i];
        const Sketch& b = doc.sketches()[i];
        QCOMPARE(b.id(), a.id());
        QCOMPARE(b.name(), a.name());
        QVERIFY(b.plane().type() == a.plane().type());
        QCOMPARE(b.nextEntityNumber(), a.nextEntityNumber());
        QCOMPARE(b.entities().size(), a.entities().size());
        for (size_t k = 0; k < a.entities().size(); ++k)
        {
            QCOMPARE(b.entities()[k].id, a.entities()[k].id);
            QCOMPARE(b.entities()[k].sketchId, a.id());
            QVERIFY(b.entities()[k].type() == a.entities()[k].type());
            const auto da = dimensionsOf(a.entities()[k]);
            const auto db = dimensionsOf(b.entities()[k]);
            for (size_t j = 0; j < da.size(); ++j)
                QCOMPARE(db[j].value, da[j].value);
        }
    }
    // Geometria OCCT reconstruída no plano correto.
    const Sketch& xz = doc.sketches()[1];
    const TopoDS_Shape circle = buildEntityShape(xz.plane(), xz.entities()[0]);
    QVERIFY(!circle.IsNull());
    QVERIFY(qAbs(ShapeInspector::boundingBox(circle).CornerMin().Y()) < 1e-6); // plano XZ: y = 0
    // Ids continuam únicos após reabrir: nova entidade recebe e-5 no sk-1.
    Sketch first = doc.sketches()[0];
    std::string id;
    QVERIFY(first.addEntity(SketchCircle{0, 0, 1}, &id).ok);
    QCOMPARE(id, std::string("e-5"));
    QVERIFY(doc.shape().IsSame(doc.shape()));
    QCOMPARE(DocumentFile::toJson(doc), DocumentFile::toJson(original));
}

void TestArquivoCadmec::compatibilidadeArquivoE51()
{
    // Arquivo real gravado pela versão E5.1 (esquema 1, sem esboços).
    const QString path = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e51_v1.cadmec");
    QVERIFY(QFile::exists(path));
    QCOMPARE(QJsonDocument::fromJson(readFile(path)).object()["schemaVersion"].toInt(), 1);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.parameters(), (PartParameters{150.0, 80.0, 25.0, 15.0}));
    QVERIFY(doc.sketches().empty());
    QCOMPARE(doc.nextSketchNumber(), 1);
    QCOMPARE(doc.operations()[0].id, std::string("op-1"));
    // Regravado na versão atual, preservando os dados.
    const QString resaved = m_dir->filePath("migrado.cadmec");
    QVERIFY(DocumentFile::save(doc, resaved).ok);
    QCOMPARE(QJsonDocument::fromJson(readFile(resaved)).object()["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    CadDocument again;
    QVERIFY(DocumentFile::load(resaved, again).ok);
    QCOMPARE(again.parameters(), doc.parameters());
    // Esquema 1 não pode conter campos de esboço.
    QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QJsonObject d = root["document"].toObject();
    d["sketches"] = QJsonArray();
    root["document"] = d;
    CadDocument rejected("mantido");
    QVERIFY(!DocumentFile::fromJson(QJsonDocument(root).toJson(), rejected).ok);
    QCOMPARE(rejected.name(), std::string("mantido"));
}

void TestArquivoCadmec::rejeitaEsbocoInvalido_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("v2 sem sketches") << sketchesModified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d.remove("sketches"); r["document"] = d; });
    QTest::newRow("v2 sem contador") << sketchesModified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d.remove("nextSketchNumber"); r["document"] = d; });
    QTest::newRow("contador de esbocos baixo") << sketchesModified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d["nextSketchNumber"] = 2; r["document"] = d; });
    QTest::newRow("plano invalido") << sketchesModified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 0); s["plane"] = "XW"; setSketch(r, 0, s); });
    QTest::newRow("id de esboco invalido") << sketchesModified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 1); s["id"] = "esboco"; setSketch(r, 1, s); });
    QTest::newRow("id de esboco duplicado") << sketchesModified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 1); s["id"] = "sk-1"; setSketch(r, 1, s); });
    QTest::newRow("nome vazio") << sketchesModified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 0); s["name"] = ""; setSketch(r, 0, s); });
    QTest::newRow("campo desconhecido no esboco") << sketchesModified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 0); s["extrude"] = 10; setSketch(r, 0, s); });
    QTest::newRow("tipo de entidade desconhecido") << sketchesModified([](QJsonObject& r) {
        setEntityField(r, 0, 0, "type", "spline"); });
    QTest::newRow("id de entidade duplicado") << sketchesModified([](QJsonObject& r) {
        setEntityField(r, 0, 1, "id", "e-1"); });
    QTest::newRow("id de entidade invalido") << sketchesModified([](QJsonObject& r) {
        setEntityField(r, 0, 1, "id", "linha"); });
    QTest::newRow("contador de entidades baixo") << sketchesModified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 0); s["nextEntityNumber"] = 3; setSketch(r, 0, s); });
    QTest::newRow("coordenada texto") << sketchesModified([](QJsonObject& r) { setEntityField(r, 0, 0, "x1", "0"); });
    QTest::newRow("coordenada ausente") << sketchesModified([](QJsonObject& r) {
        setEntityField(r, 0, 0, "y2", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("campo extra na entidade") << sketchesModified([](QJsonObject& r) {
        setEntityField(r, 0, 2, "width", 3.0); });
    QTest::newRow("linha degenerada") << sketchesModified([](QJsonObject& r) { setEntityField(r, 0, 0, "x2", 0.0); });
    QTest::newRow("largura negativa") << sketchesModified([](QJsonObject& r) {
        setEntityField(r, 0, 1, "width", -40.0); });
    QTest::newRow("raio zero") << sketchesModified([](QJsonObject& r) { setEntityField(r, 0, 2, "radius", 0.0); });
    QTest::newRow("coordenada enorme") << sketchesModified([](QJsonObject& r) { setEntityField(r, 1, 0, "cx", 1e7); });
}

void TestArquivoCadmec::rejeitaEsbocoInvalido()
{
    QFETCH(QByteArray, content);
    CadDocument active = documentWithSketches();
    QVERIFY(active.setParameters({111.0, 55.0, 22.0, 9.0}).ok);
    const size_t sketchesBefore = active.sketches().size();
    const FileResult r = DocumentFile::fromJson(content, active);
    qInfo("%s -> %s", QTest::currentDataTag(), qPrintable(r.message));
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    QCOMPARE(active.sketches().size(), sketchesBefore);
    QCOMPARE(active.parameters(), (PartParameters{111.0, 55.0, 22.0, 9.0}));
}

void TestArquivoCadmec::documentoVazio()
{
    const QString path = m_dir->filePath("vazio.cadmec");
    QVERIFY(DocumentFile::save(CadDocument(), path).ok);
    const QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    const QJsonObject d = root["document"].toObject();
    QCOMPARE(d["name"].toString(), QStringLiteral("Sem título"));
    QVERIFY(d["operations"].isArray() && d["operations"].toArray().isEmpty());
    QVERIFY(d["sketches"].isArray() && d["sketches"].toArray().isEmpty());

    CadDocument doc = CadDocument::withTestPart("ativo");
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QVERIFY(!doc.hasPart());
    QVERIFY(doc.shape().IsNull());
    QVERIFY(doc.sketches().empty());
    QVERIFY(!doc.isModified());
    QCOMPARE(DocumentFile::toJson(doc), DocumentFile::toJson(CadDocument()));
}

void TestArquivoCadmec::documentoSomenteComEsbocos()
{
    using namespace cad::sketch;
    CadDocument doc("Somente esboços");
    std::string error;
    Sketch s = doc.createSketch(PlaneType::XY);
    QVERIFY(s.addEntity(SketchRectangle{0.0, 0.0, 80.0, 40.0}).ok);
    QVERIFY(s.addEntity(SketchCircle{40.0, 20.0, 10.0}).ok);
    QVERIFY(doc.commitSketch(s, error));
    QVERIFY(!doc.hasPart() && doc.shape().IsNull()); // esboços não criam sólido

    const QString path = m_dir->filePath("somente_esbocos.cadmec");
    QVERIFY(DocumentFile::save(doc, path).ok);
    CadDocument loaded;
    const FileResult r = DocumentFile::load(path, loaded);
    QVERIFY2(r.ok, qPrintable(r.message));
    QVERIFY(!loaded.hasPart());
    QVERIFY(loaded.shape().IsNull());
    QCOMPARE(loaded.sketches().size(), size_t(1));
    QCOMPARE(loaded.sketches()[0].entities().size(), size_t(2));
    QVERIFY(loaded.sketches()[0].entities()[0].type() == EntityType::Rectangle);
    QCOMPARE(std::get<SketchCircle>(loaded.sketches()[0].entities()[1].geometry).radius, 10.0);
    QCOMPARE(DocumentFile::toJson(loaded), DocumentFile::toJson(doc));
}

void TestArquivoCadmec::arquivoE52ComBlocoEFuro()
{
    // Arquivo real gravado pela versão E5.2 (esquema 2): bloco, furo e um esboço.
    const QString path = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e52_v2.cadmec");
    const QByteArray original = readFile(path);
    QCOMPARE(QJsonDocument::fromJson(original).object()["schemaVersion"].toInt(), 2);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QVERIFY(doc.hasPart());
    QCOMPARE(doc.parameters(), PartParameters());
    QCOMPARE(ShapeInspector::inspect(doc.shape()).faces, 7);
    QCOMPARE(doc.sketches().size(), size_t(1));
    QCOMPARE(doc.sketches()[0].entities().size(), size_t(3));
    QCOMPARE(readFile(path), original); // abrir não altera o arquivo
}

void TestArquivoCadmec::operacoesVaziasPorVersao()
{
    // Esquemas 1 e 2 exigem bloco + furo; o esquema 3 aceita lista vazia, mas não parcial.
    auto withOps = [](int schema, int count) {
        QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(CadDocument::withTestPart())).object();
        root["schemaVersion"] = schema;
        QJsonObject d = root["document"].toObject();
        QJsonArray ops = d["operations"].toArray();
        while (ops.size() > count)
            ops.removeLast();
        d["operations"] = ops;
        if (schema < 4)
            d.remove("nextOperationNumber");
        if (schema == 1)
        {
            d.remove("sketches");
            d.remove("nextSketchNumber");
        }
        root["document"] = d;
        return QJsonDocument(root).toJson();
    };
    CadDocument doc;
    QVERIFY(!DocumentFile::fromJson(withOps(1, 0), doc).ok);
    QVERIFY(!DocumentFile::fromJson(withOps(2, 0), doc).ok);
    QVERIFY(!DocumentFile::fromJson(withOps(3, 1), doc).ok);
    QVERIFY(DocumentFile::fromJson(withOps(3, 0), doc).ok);
    QVERIFY(!doc.hasPart());
    QVERIFY(DocumentFile::fromJson(withOps(3, 2), doc).ok);
    QVERIFY(doc.hasPart());
    QVERIFY(DocumentFile::fromJson(withOps(2, 2), doc).ok);
    QVERIFY(DocumentFile::fromJson(withOps(1, 2), doc).ok);
}

namespace {

// Documento com bloco extrudado (sk-1, op-1) e cilindro invertido (sk-2, op-2).
CadDocument documentWithExtrusions()
{
    using namespace cad::sketch;
    CadDocument doc("Extrusões");
    std::string error;
    Sketch a = doc.createSketch(PlaneType::XY);
    a.addEntity(SketchRectangle{0.0, 0.0, 100.0, 60.0});
    doc.commitSketch(a, error);
    Sketch b = doc.createSketch(PlaneType::XZ);
    b.addEntity(SketchCircle{200.0, 10.0, 10.0});
    doc.commitSketch(b, error);
    doc.addExtrusion({"Bloco", a.id(), 20.0, false});
    doc.addExtrusion({"", b.id(), 35.0, true});
    return doc;
}

QJsonObject extrudeOp(const QJsonObject& root, int i)
{
    return root["document"].toObject()["operations"].toArray()[i].toObject();
}

template <typename F>
QByteArray extrusionsModified(F edit)
{
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(documentWithExtrusions())).object();
    edit(root);
    return QJsonDocument(root).toJson();
}

} // namespace

void TestArquivoCadmec::salvarEAbrirExtrusao()
{
    const CadDocument original = documentWithExtrusions();
    QCOMPARE(original.operations().size(), size_t(2));
    const QString path = m_dir->filePath("extrusoes.cadmec");
    QVERIFY(DocumentFile::save(original, path).ok);

    const QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["schemaVersion"].toInt(), 4);
    const QJsonObject d = root["document"].toObject();
    QCOMPARE(d["nextOperationNumber"].toInt(), 3);
    const QJsonObject e = d["operations"].toArray()[1].toObject();
    QCOMPARE(e["type"].toString(), QStringLiteral("extrude"));
    QCOMPARE(e["id"].toString(), QStringLiteral("op-2"));
    QCOMPARE(e["name"].toString(), QStringLiteral("Extrusão 2"));
    QCOMPARE(e["sketchId"].toString(), QStringLiteral("sk-2"));
    QCOMPARE(e["reversed"].toBool(), true);
    QCOMPARE(e["parameters"].toObject()["depth"].toDouble(), 35.0);
    QVERIFY(!readFile(path).contains("triang")); // nenhuma malha/objeto gráfico salvo

    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(2));
    QCOMPARE(doc.solidCount(), 2);
    QCOMPARE(doc.operations()[0].name, std::string("Bloco"));
    QCOMPARE(doc.extrudeParameters("op-2").depth, 35.0);
    QVERIFY(doc.extrudeParameters("op-2").reversed);
    QCOMPARE(doc.nextOperationNumber(), 3);
    // Geometria reconstruída a partir dos parâmetros: mesmo volume.
    const double expected = 120000.0 + 3.14159265358979323846 * 100.0 * 35.0;
    QVERIFY(qAbs(ShapeInspector::volume(doc.shape()) - expected) < 1e-6);
    QCOMPARE(DocumentFile::toJson(doc), DocumentFile::toJson(original));
}

void TestArquivoCadmec::arquivoE521SomenteEsbocos()
{
    // Arquivo real gravado pela E5.2.1 (esquema 3, somente esboços).
    const QString path = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e521_v3.cadmec");
    const QByteArray original = readFile(path);
    QCOMPARE(QJsonDocument::fromJson(original).object()["schemaVersion"].toInt(), 3);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QVERIFY(doc.operations().empty() && doc.shape().IsNull());
    QCOMPARE(doc.sketches().size(), size_t(1));
    QCOMPARE(doc.nextOperationNumber(), 1);
    QCOMPARE(readFile(path), original);
    // O esboço antigo tem dois contornos (retângulo + círculo): a extrusão é recusada com mensagem
    // clara e o documento permanece intacto e regravável no esquema atual.
    const auto ext = doc.addExtrusion({"", doc.sketches()[0].id(), 10.0, false});
    QVERIFY(!ext.ok);
    QVERIFY2(ext.message.find("contornos fechados") != std::string::npos, ext.message.c_str());
    QVERIFY(doc.operations().empty());
    QVERIFY(DocumentFile::save(doc, m_dir->filePath("e521_regravado.cadmec")).ok);
}

void TestArquivoCadmec::rejeitaExtrusaoInvalida_data()
{
    QTest::addColumn<QByteArray>("content");
    auto setOpField = [](QJsonObject& r, int i, const char* key, const QJsonValue& value) {
        QJsonObject o = extrudeOp(r, i);
        if (value.isUndefined())
            o.remove(key);
        else
            o[key] = value;
        setOp(r, i, o);
    };
    QTest::newRow("esboco inexistente") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 0, "sketchId", "sk-9"); });
    QTest::newRow("profundidade zero") << extrusionsModified([&](QJsonObject& r) { setParam(r, 0, "depth", 0.0); });
    QTest::newRow("profundidade texto") << extrusionsModified([&](QJsonObject& r) { setParam(r, 0, "depth", "20"); });
    QTest::newRow("sem reversed") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 0, "reversed", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("reversed texto") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 0, "reversed", "sim"); });
    QTest::newRow("campo desconhecido") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 1, "mesh", "x"); });
    QTest::newRow("nome vazio") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 0, "name", "  "); });
    QTest::newRow("id duplicado") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 1, "id", "op-1"); });
    QTest::newRow("tipo desconhecido") << extrusionsModified([&](QJsonObject& r) { setOpField(r, 1, "type", "revolve"); });
    QTest::newRow("contador baixo") << extrusionsModified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d["nextOperationNumber"] = 2; r["document"] = d; });
    QTest::newRow("sem contador v4") << extrusionsModified([](QJsonObject& r) {
        QJsonObject d = r["document"].toObject(); d.remove("nextOperationNumber"); r["document"] = d; });
    QTest::newRow("extrusao em esquema 3") << extrusionsModified([](QJsonObject& r) {
        r["schemaVersion"] = 3; QJsonObject d = r["document"].toObject(); d.remove("nextOperationNumber"); r["document"] = d; });
    QTest::newRow("perfil aberto no arquivo") << extrusionsModified([](QJsonObject& r) {
        // Troca o retângulo do sk-1 por uma linha: a extrusão op-1 não pode ser reconstruída.
        QJsonObject s = sketchObject(r, 0);
        QJsonArray ents;
        ents.append(QJsonObject{{"id", "e-1"}, {"type", "line"}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 10.0}, {"y2", 0.0}});
        s["entities"] = ents;
        setSketch(r, 0, s); });
}

void TestArquivoCadmec::rejeitaExtrusaoInvalida()
{
    QFETCH(QByteArray, content);
    CadDocument active = documentWithExtrusions();
    const TopoDS_Shape before = active.shape();
    const FileResult r = DocumentFile::fromJson(content, active);
    qInfo("%s -> %s", QTest::currentDataTag(), qPrintable(r.message));
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    QVERIFY(active.shape().IsSame(before));
    QCOMPARE(active.operations().size(), size_t(2));
}

QTEST_GUILESS_MAIN(TestArquivoCadmec)
#include "test_arquivo_cadmec.moc"
