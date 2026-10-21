// Formato nativo .cadmec (E5.1): salvar, abrir, validar e rejeitar arquivos inválidos.
#include "document/DocumentFile.h"
#include "modeling/FaceNaming.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchShapes.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

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
    // E5.4
    void salvarEAbrirBooleanas();
    void arquivoE53Esquema4();
    void rejeitaBooleanaInvalida_data();
    void rejeitaBooleanaInvalida();
    // E5.5
    void salvarEAbrirE55();
    void arquivoE54Esquema5();
    void rejeitaE55Invalido_data();
    void rejeitaE55Invalido();
    // E5.6
    void salvarEAbrirE56();
    void arquivoE55Esquema6();
    void rejeitaE56Invalido_data();
    void rejeitaE56Invalido();

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
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
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

namespace {

// Bloco (op-1) − cilindro (op-2) = op-3; união (op-5) do resultado com um terceiro bloco (op-4).
CadDocument documentWithBooleans()
{
    using namespace cad::sketch;
    CadDocument doc("Booleanas");
    std::string error;
    Sketch a = doc.createSketch(PlaneType::XZ);
    a.addEntity(SketchRectangle{0.0, 5.0, 100.0, 20.0});
    doc.commitSketch(a, error);
    Sketch b = doc.createSketch(PlaneType::XY);
    b.addEntity(SketchCircle{50.0, -30.0, 10.0});
    doc.commitSketch(b, error);
    Sketch c = doc.createSketch(PlaneType::XY);
    c.addEntity(SketchRectangle{100.0, -60.0, 20.0, 60.0});
    doc.commitSketch(c, error);
    doc.addExtrusion({"Bloco", a.id(), 60.0, false});
    doc.addExtrusion({"Cilindro", b.id(), 30.0, false});
    doc.addBoolean({"Bloco - Cilindro", cad::modeling::BooleanKind::Cut, "op-1", "op-2"});
    doc.addExtrusion({"Aba", c.id(), 25.0, false});
    doc.addBoolean({"", cad::modeling::BooleanKind::Fuse, "op-3", "op-4"});
    return doc;
}

template <typename F>
QByteArray booleansModified(F edit)
{
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(documentWithBooleans())).object();
    edit(root);
    return QJsonDocument(root).toJson();
}

QJsonObject opAt(const QJsonObject& root, int i)
{
    return root["document"].toObject()["operations"].toArray()[i].toObject();
}

} // namespace

void TestArquivoCadmec::salvarEAbrirBooleanas()
{
    const CadDocument original = documentWithBooleans();
    QCOMPARE(original.operations().size(), size_t(5));
    QCOMPARE(original.activeBodies(), std::vector<std::string>{"op-5"});
    const QString path = m_dir->filePath("booleanas.cadmec");
    QVERIFY(DocumentFile::save(original, path).ok);

    const QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    const QJsonObject cut = opAt(root, 2);
    QCOMPARE(cut["type"].toString(), QStringLiteral("boolean"));
    QCOMPARE(cut["id"].toString(), QStringLiteral("op-3"));
    QCOMPARE(cut["name"].toString(), QStringLiteral("Bloco - Cilindro"));
    QCOMPARE(cut["operation"].toString(), QStringLiteral("cut"));
    QCOMPARE(cut["operandA"].toString(), QStringLiteral("op-1"));
    QCOMPARE(cut["operandB"].toString(), QStringLiteral("op-2"));
    QVERIFY(!cut.contains("parameters"));
    QCOMPARE(opAt(root, 4)["operation"].toString(), QStringLiteral("fuse"));
    QCOMPARE(opAt(root, 4)["name"].toString(), QStringLiteral("União 1"));

    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(5));
    QCOMPARE(doc.activeBodies(), std::vector<std::string>{"op-5"});
    QCOMPARE(doc.consumerOf("op-1"), std::string("op-3"));
    QCOMPARE(doc.consumerOf("op-3"), std::string("op-5"));
    QCOMPARE(doc.solidCount(), 1); // operandos ocultos não duplicam o resultado
    // Reconstruído dos parâmetros: (bloco − cilindro) ∪ aba (aba 20 x 60 x 25 encostada em x = 100).
    // A aba (x em [100, 120]) encosta na face x = 100 do bloco, sem sobreposição.
    const double expected = 120000.0 - 2000.0 * 3.14159265358979323846 + 20.0 * 60.0 * 25.0;
    QVERIFY2(qAbs(ShapeInspector::volume(doc.shape()) - expected) < 1e-6,
             qPrintable(QString::number(ShapeInspector::volume(doc.shape()), 'f', 4)));
    QCOMPARE(DocumentFile::toJson(doc), DocumentFile::toJson(original));
    QCOMPARE(doc.nextOperationNumber(), 6);
}

void TestArquivoCadmec::arquivoE53Esquema4()
{
    // Arquivo real gravado na sessão Windows da E5.3 (esquema 4, uma extrusão).
    const QString path = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e53_v4.cadmec");
    const QByteArray original = readFile(path);
    QCOMPARE(QJsonDocument::fromJson(original).object()["schemaVersion"].toInt(), 4);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(1));
    QVERIFY(qAbs(ShapeInspector::volume(doc.shape()) - 53.2 * 32.1 * 40.0) < 1e-6);
    QCOMPARE(readFile(path), original);
    // Booleanas não pertencem ao esquema 4.
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(documentWithBooleans())).object();
    root["schemaVersion"] = 4;
    // Campos do esquema 7 removidos das extrusões: só a booleana fica fora do esquema 4.
    QJsonObject d4 = root["document"].toObject();
    QJsonArray ops4 = d4["operations"].toArray();
    for (int i = 0; i < ops4.size(); ++i)
    {
        QJsonObject o = ops4[i].toObject();
        for (const char* key : {"mode", "targetId", "endCondition"})
            o.remove(key);
        ops4[i] = o;
    }
    d4["operations"] = ops4;
    root["document"] = d4;
    CadDocument active = documentWithBooleans();
    const FileResult old = DocumentFile::fromJson(QJsonDocument(root).toJson(), active);
    qInfo("booleana no esquema 4 -> %s", qPrintable(old.message));
    QVERIFY(!old.ok && old.message.contains("extrude"));
}

void TestArquivoCadmec::rejeitaBooleanaInvalida_data()
{
    QTest::addColumn<QByteArray>("content");
    auto setField = [](QJsonObject& r, int i, const char* key, const QJsonValue& value) {
        QJsonObject o = opAt(r, i);
        if (value.isUndefined())
            o.remove(key);
        else
            o[key] = value;
        setOp(r, i, o);
    };
    QTest::newRow("operando inexistente") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operandB", "op-9"); });
    QTest::newRow("operando posterior") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operandB", "op-4"); });
    QTest::newRow("operando proprio") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operandA", "op-3"); });
    QTest::newRow("A igual a B") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operandB", "op-1"); });
    QTest::newRow("corpo consumido duas vezes") << booleansModified([&](QJsonObject& r) { setField(r, 4, "operandA", "op-1"); });
    QTest::newRow("sem operandA") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operandA", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("operandA numerico") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operandA", 1); });
    QTest::newRow("operacao desconhecida") << booleansModified([&](QJsonObject& r) { setField(r, 2, "operation", "xor"); });
    QTest::newRow("campo desconhecido") << booleansModified([&](QJsonObject& r) { setField(r, 2, "parameters", QJsonObject{}); });
    QTest::newRow("nome vazio") << booleansModified([&](QJsonObject& r) { setField(r, 2, "name", " "); });
    QTest::newRow("interseccao vazia") << booleansModified([&](QJsonObject& r) {
        setField(r, 2, "operation", "common");
        // Cilindro deslocado para fora do bloco: a interseção não tem volume.
        QJsonObject s = sketchObject(r, 1);
        QJsonArray e = s["entities"].toArray();
        QJsonObject c = e[0].toObject();
        c["cx"] = 500.0;
        e[0] = c;
        s["entities"] = e;
        setSketch(r, 1, s); });
    QTest::newRow("bloco base como operando") << booleansModified([](QJsonObject& r) {
        // Documento com a peça de exemplo: o bloco base (consumido pelo furo) não é corpo.
        CadDocument ex = CadDocument::withTestPart();
        r = QJsonDocument::fromJson(DocumentFile::toJson(ex)).object();
        QJsonObject d = r["document"].toObject();
        QJsonArray ops = d["operations"].toArray();
        ops.append(QJsonObject{{"id", "op-3"}, {"type", "boolean"}, {"name", "X"}, {"operation", "fuse"},
                               {"operandA", "op-1"}, {"operandB", "op-2"}});
        d["operations"] = ops;
        d["nextOperationNumber"] = 4;
        r["document"] = d; });
}

void TestArquivoCadmec::rejeitaBooleanaInvalida()
{
    QFETCH(QByteArray, content);
    CadDocument active = documentWithBooleans();
    const TopoDS_Shape before = active.shape();
    const FileResult r = DocumentFile::fromJson(content, active);
    qInfo("%s -> %s", QTest::currentDataTag(), qPrintable(r.message));
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    QVERIFY(active.shape().IsSame(before));
    QCOMPARE(active.operations().size(), size_t(5));
}

namespace {

// Esboço XY com retângulo + Fillet 2D R10 (canto 2) + Chamfer 2D 5 (canto 0); esboço separado com
// um arco; extrusão 20 mm; Fillet 3D R3 na aresta superior frontal; Chamfer 3D 2 na aresta
// inferior frontal do resultado.
CadDocument documentE55()
{
    using namespace cad::sketch;
    CadDocument doc("Acabamentos");
    std::string error;
    Sketch s = doc.createSketch(PlaneType::XY);
    s.addEntity(SketchRectangle{0.0, 0.0, 100.0, 60.0});
    s.addEntity(SketchFillet{"e-1", "e-1", 2, 10.0});
    s.addEntity(SketchChamfer{"e-1", "e-1", 0, 5.0});
    doc.commitSketch(s, error);
    Sketch arc = doc.createSketch(PlaneType::XZ);
    arc.addEntity(SketchArc{150.0, 10.0, 8.0, 30.0, 200.0, true});
    doc.commitSketch(arc, error);
    std::string ext, fil, cha;
    doc.addExtrusion({"Bloco", s.id(), 20.0, false}, &ext);
    auto edgeAt = [](const TopoDS_Shape& shape, double x, double y, double z) {
        for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
        {
            BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
            if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(x, y, z)) < 1e-6)
                return TopoDS::Edge(ex.Current());
        }
        return TopoDS_Edge();
    };
    // Aresta superior frontal: de x = 5 (chanfro 2D) a x = 100.
    doc.addFinish(OperationType::Fillet3D,
                  {"", ext, 3.0, doc.edgeSignatures(ext, {edgeAt(doc.shape(), 52.5, 0.0, 20.0)})}, &fil);
    // Aresta inferior frontal (x de 5 a 100), sem arestas tangentes encadeadas.
    doc.addFinish(OperationType::Chamfer3D,
                  {"", fil, 2.0, doc.edgeSignatures(fil, {edgeAt(doc.shape(), 52.5, 0.0, 0.0)})}, &cha);
    return doc;
}

template <typename F>
QByteArray e55Modified(F edit)
{
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(documentE55())).object();
    edit(root);
    return QJsonDocument(root).toJson();
}

QJsonObject entityAt(QJsonObject& root, int sketch, int i)
{
    return sketchObject(root, sketch)["entities"].toArray()[i].toObject();
}

void setEntity(QJsonObject& root, int sketch, int i, const QJsonObject& value)
{
    QJsonObject s = sketchObject(root, sketch);
    QJsonArray e = s["entities"].toArray();
    e[i] = value;
    s["entities"] = e;
    setSketch(root, sketch, s);
}

} // namespace

void TestArquivoCadmec::salvarEAbrirE55()
{
    const CadDocument original = documentE55();
    QCOMPARE(original.operations().size(), size_t(3));
    QCOMPARE(original.activeBodies(), std::vector<std::string>{"op-3"});
    const QString path = m_dir->filePath("e55.cadmec");
    QVERIFY(DocumentFile::save(original, path).ok);

    QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    const QJsonObject fillet = entityAt(root, 0, 1);
    QCOMPARE(fillet["type"].toString(), QStringLiteral("fillet"));
    QCOMPARE(fillet["entityA"].toString(), QStringLiteral("e-1"));
    QCOMPARE(fillet["corner"].toInt(), 2);
    QCOMPARE(fillet["radius"].toDouble(), 10.0);
    QCOMPARE(entityAt(root, 0, 2)["type"].toString(), QStringLiteral("chamfer"));
    QCOMPARE(entityAt(root, 0, 2)["distance"].toDouble(), 5.0);
    const QJsonObject arc = entityAt(root, 1, 0);
    QCOMPARE(arc["type"].toString(), QStringLiteral("arc"));
    QCOMPARE(arc["startAngle"].toDouble(), 30.0);
    QCOMPARE(arc["endAngle"].toDouble(), 200.0);
    QCOMPARE(arc["clockwise"].toBool(), true);
    const QJsonObject f3 = opAt(root, 1);
    QCOMPARE(f3["type"].toString(), QStringLiteral("fillet3d"));
    QCOMPARE(f3["sourceId"].toString(), QStringLiteral("op-1"));
    QCOMPARE(f3["parameters"].toObject()["radius"].toDouble(), 3.0);
    const QJsonObject edge = f3["edges"].toArray()[0].toObject();
    QCOMPARE(edge["curve"].toString(), QStringLiteral("line"));
    QCOMPARE(edge["normals"].toArray().size(), 2);
    QCOMPARE(opAt(root, 2)["type"].toString(), QStringLiteral("chamfer3d"));
    QCOMPARE(opAt(root, 2)["parameters"].toObject()["distance"].toDouble(), 2.0);
    QVERIFY(!readFile(path).contains("triang"));

    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(3));
    QCOMPARE(doc.activeBodies(), std::vector<std::string>{"op-3"});
    QVERIFY(qAbs(ShapeInspector::volume(doc.shape()) - ShapeInspector::volume(original.shape())) < 1e-6);
    QVERIFY(ShapeInspector::isValid(doc.shape()));
    QCOMPARE(doc.sketches()[0].entities().size(), size_t(3));
    QCOMPARE(DocumentFile::toJson(doc), DocumentFile::toJson(original));
    qInfo("volume reconstruido: %.4f", ShapeInspector::volume(doc.shape()));
}

void TestArquivoCadmec::arquivoE54Esquema5()
{
    const QString path = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e54_v5.cadmec");
    const QByteArray original = readFile(path);
    QCOMPARE(QJsonDocument::fromJson(original).object()["schemaVersion"].toInt(), 5);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(3));
    QCOMPARE(doc.operations()[2].type, OperationType::Boolean);
    QCOMPARE(doc.solidCount(), 1);
    QVERIFY(qAbs(ShapeInspector::volume(doc.shape()) - 69710.96) < 0.01);
    QCOMPARE(readFile(path), original);
}

void TestArquivoCadmec::rejeitaE55Invalido_data()
{
    QTest::addColumn<QByteArray>("content");
    auto setEntityField = [](QJsonObject& r, int sk, int i, const char* key, const QJsonValue& v) {
        QJsonObject e = entityAt(r, sk, i);
        if (v.isUndefined())
            e.remove(key);
        else
            e[key] = v;
        setEntity(r, sk, i, e);
    };
    auto setOpField = [](QJsonObject& r, int i, const char* key, const QJsonValue& v) {
        QJsonObject o = opAt(r, i);
        if (v.isUndefined())
            o.remove(key);
        else
            o[key] = v;
        setOp(r, i, o);
    };
    auto setEdgeField = [](QJsonObject& r, int i, const char* key, const QJsonValue& v) {
        QJsonObject o = opAt(r, i);
        QJsonArray edges = o["edges"].toArray();
        QJsonObject e = edges[0].toObject();
        if (v.isUndefined())
            e.remove(key);
        else
            e[key] = v;
        edges[0] = e;
        o["edges"] = edges;
        setOp(r, i, o);
    };
    QTest::newRow("arco no esquema 5") << e55Modified([](QJsonObject& r) {
        // Só a extrusão permanece: o motivo da recusa é a entidade "arc"/"fillet" no esquema 5.
        r["schemaVersion"] = 5;
        QJsonObject d = r["document"].toObject();
        QJsonArray ops = d["operations"].toArray();
        while (ops.size() > 1)
            ops.removeLast();
        d["operations"] = ops;
        r["document"] = d; });
    QTest::newRow("arco varredura nula") << e55Modified([&](QJsonObject& r) { setEntityField(r, 1, 0, "endAngle", 30.0); });
    QTest::newRow("arco raio zero") << e55Modified([&](QJsonObject& r) { setEntityField(r, 1, 0, "radius", 0.0); });
    QTest::newRow("arco sentido texto") << e55Modified([&](QJsonObject& r) { setEntityField(r, 1, 0, "clockwise", "sim"); });
    QTest::newRow("fillet referencia inexistente") << e55Modified([&](QJsonObject& r) {
        setEntityField(r, 0, 1, "entityA", "e-9");
        setEntityField(r, 0, 1, "entityB", "e-9"); });
    QTest::newRow("fillet raio inviavel") << e55Modified([&](QJsonObject& r) { setEntityField(r, 0, 1, "radius", 80.0); });
    QTest::newRow("fillet canto fracionario") << e55Modified([&](QJsonObject& r) { setEntityField(r, 0, 1, "corner", 1.5); });
    QTest::newRow("fillet canto fora") << e55Modified([&](QJsonObject& r) { setEntityField(r, 0, 1, "corner", 7); });
    QTest::newRow("chamfer sem distancia") << e55Modified([&](QJsonObject& r) { setEntityField(r, 0, 2, "distance", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("chamfer mesmo canto do fillet") << e55Modified([&](QJsonObject& r) { setEntityField(r, 0, 2, "corner", 2); });
    QTest::newRow("fillet3d no esquema 5") << e55Modified([](QJsonObject& r) {
        r["schemaVersion"] = 5;
        // Remove as entidades E5.5 para que só a operação seja o problema.
        QJsonObject d = r["document"].toObject();
        d["sketches"] = QJsonArray{};
        r["document"] = d; });
    QTest::newRow("fillet3d raio zero") << e55Modified([&](QJsonObject& r) {
        QJsonObject o = opAt(r, 1); o["parameters"] = QJsonObject{{"radius", 0.0}}; setOp(r, 1, o); });
    QTest::newRow("fillet3d raio excessivo") << e55Modified([&](QJsonObject& r) {
        QJsonObject o = opAt(r, 1); o["parameters"] = QJsonObject{{"radius", 40.0}}; setOp(r, 1, o); });
    QTest::newRow("fillet3d sem arestas") << e55Modified([&](QJsonObject& r) { setOpField(r, 1, "edges", QJsonArray{}); });
    QTest::newRow("fillet3d origem inexistente") << e55Modified([&](QJsonObject& r) { setOpField(r, 1, "sourceId", "op-9"); });
    QTest::newRow("chamfer3d origem consumida") << e55Modified([&](QJsonObject& r) { setOpField(r, 2, "sourceId", "op-1"); });
    QTest::newRow("aresta curva invalida") << e55Modified([&](QJsonObject& r) { setEdgeField(r, 1, "curve", "spline"); });
    QTest::newRow("aresta sem normais") << e55Modified([&](QJsonObject& r) { setEdgeField(r, 1, "normals", QJsonArray{}); });
    QTest::newRow("aresta campo desconhecido") << e55Modified([&](QJsonObject& r) { setEdgeField(r, 1, "index", 3); });
    QTest::newRow("aresta nao remapeavel") << e55Modified([&](QJsonObject& r) {
        // Direção vertical com as normais da aresta superior frontal: nenhuma aresta tem esse papel.
        setEdgeField(r, 1, "direction", QJsonArray{0.0, 0.0, 1.0}); });
    QTest::newRow("aresta ponto medio nao numerico") << e55Modified([&](QJsonObject& r) { setEdgeField(r, 1, "mid", QJsonArray{1.0, "x", 0.0}); });
}

void TestArquivoCadmec::rejeitaE55Invalido()
{
    QFETCH(QByteArray, content);
    CadDocument active = documentE55();
    const TopoDS_Shape before = active.shape();
    const FileResult r = DocumentFile::fromJson(content, active);
    qInfo("%s -> %s", QTest::currentDataTag(), qPrintable(r.message));
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    QVERIFY(active.shape().IsSame(before));
    QCOMPARE(active.operations().size(), size_t(3));
}

namespace {

TopoDS_Face planarFace(const TopoDS_Shape& body, const gp_Dir& normal, const gp_Pnt& near)
{
    TopoDS_Face best;
    double bestD = 1e300;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        const auto pl = cad::modeling::planarFacePlane(f);
        if (pl && pl->Axis().Direction().Angle(normal) < 1e-6)
        {
            const double d = cad::modeling::describeFace(f).centroid.Distance(near);
            if (d < bestD)
            {
                bestD = d;
                best = f;
            }
        }
    }
    return best;
}

// Fluxo E5.6: bloco (novo corpo) → esboço na face superior com círculo → corte atravessando
// tudo → esboço na face frontal → saliência 10 mm → Chamfer 3D duas distâncias; esboço 1 com
// Chamfer 2D distância–ângulo.
CadDocument documentE56()
{
    using namespace cad::sketch;
    CadDocument doc("Faces");
    std::string error;
    Sketch s = doc.createSketch(PlaneType::XY);
    s.addEntity(SketchRectangle{0.0, 0.0, 100.0, 60.0});
    s.addEntity(SketchChamfer{"e-1", "e-1", 0, 5.0, ChamferMode::DistanceAngle, 0.0, 30.0, false});
    doc.commitSketch(s, error);
    std::string b, cut, boss, ch;
    doc.addExtrusion({"Bloco", s.id(), 20.0}, &b);
    auto top = doc.createFaceSketch(b, planarFace(doc.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)), error);
    top->addEntity(SketchCircle{50.0, 30.0, 10.0});
    doc.commitSketch(*top, error);
    doc.addExtrusion({"", top->id(), 1.0, false, ExtrudeMode::Cut, b, EndCondition::ThroughAll}, &cut);
    auto front = doc.createFaceSketch(cut, planarFace(doc.shape(), gp_Dir(0, -1, 0), gp_Pnt(50, 0, 10)), error);
    front->addEntity(SketchRectangle{60.0, 2.0, 20.0, 15.0});
    doc.commitSketch(*front, error);
    doc.addExtrusion({"", front->id(), 10.0, false, ExtrudeMode::Boss, cut}, &boss);
    TopoDS_Edge backTop;
    for (TopExp_Explorer ex(doc.shape(), TopAbs_EDGE); ex.More(); ex.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(50, 60, 20)) < 1e-6)
            backTop = TopoDS::Edge(ex.Current());
    }
    FinishParameters fp{"", boss, 3.0, doc.edgeSignatures(boss, {backTop})};
    fp.chamferMode = Chamfer3DMode::TwoDistances;
    fp.value2 = 6.0;
    fp.referenceNormals = doc.referenceNormals(boss, {backTop}, false);
    doc.addFinish(OperationType::Chamfer3D, fp, &ch);
    return doc;
}

template <typename F>
QByteArray e56Modified(F edit)
{
    QJsonObject root = QJsonDocument::fromJson(DocumentFile::toJson(documentE56())).object();
    edit(root);
    return QJsonDocument(root).toJson();
}

} // namespace

void TestArquivoCadmec::salvarEAbrirE56()
{
    const CadDocument original = documentE56();
    QCOMPARE(original.operations().size(), size_t(4));
    QCOMPARE(original.activeBodies(), std::vector<std::string>{"op-4"});
    const QString path = m_dir->filePath("e56.cadmec");
    QVERIFY(DocumentFile::save(original, path).ok);
    const QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(root["schemaVersion"].toInt(), DocumentFile::kSchemaVersion);
    QCOMPARE(sketchObject(const_cast<QJsonObject&>(root), 1)["plane"].toString(), QStringLiteral("face"));
    const QJsonObject support = sketchObject(const_cast<QJsonObject&>(root), 1)["support"].toObject();
    QCOMPARE(support["bodyId"].toString(), QStringLiteral("op-1"));
    QCOMPARE(support["face"].toObject()["normal"].toArray()[2].toDouble(), 1.0);
    QCOMPARE(support["frame"].toObject()["origin"].toArray()[2].toDouble(), 20.0);
    const QJsonObject cut = opAt(root, 1);
    QCOMPARE(cut["mode"].toString(), QStringLiteral("cut"));
    QCOMPARE(cut["targetId"].toString(), QStringLiteral("op-1"));
    QCOMPARE(cut["endCondition"].toString(), QStringLiteral("throughAll"));
    QCOMPARE(opAt(root, 0)["mode"].toString(), QStringLiteral("newBody"));
    QVERIFY(!opAt(root, 0).contains("targetId"));
    QCOMPARE(opAt(root, 2)["mode"].toString(), QStringLiteral("boss"));
    const QJsonObject ch = opAt(root, 3);
    QCOMPARE(ch["mode"].toString(), QStringLiteral("twoDistances"));
    QCOMPARE(ch["parameters"].toObject()["distance2"].toDouble(), 6.0);
    QCOMPARE(ch["referenceNormals"].toArray().size(), 1);
    const QJsonObject chamfer2d = entityAt(const_cast<QJsonObject&>(root), 0, 1);
    QCOMPARE(chamfer2d["mode"].toString(), QStringLiteral("distanceAngle"));
    QCOMPARE(chamfer2d["angle"].toDouble(), 30.0);

    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(4));
    QCOMPARE(doc.activeBodies(), std::vector<std::string>{"op-4"});
    QVERIFY(doc.findSketch("sk-2")->support() && doc.findSketch("sk-2")->plane().type() == cad::sketch::PlaneType::Face);
    QVERIFY(qAbs(ShapeInspector::volume(doc.shape()) - ShapeInspector::volume(original.shape())) < 1e-6);
    QVERIFY(ShapeInspector::isValid(doc.shape()));
    QCOMPARE(DocumentFile::toJson(doc), DocumentFile::toJson(original));
    qInfo("volume reconstruido: %.4f", ShapeInspector::volume(doc.shape()));
}

void TestArquivoCadmec::arquivoE55Esquema6()
{
    const QString path = QStringLiteral(CADMEC_TEST_DATA_DIR "/documento_e55_v6.cadmec");
    const QByteArray original = readFile(path);
    QCOMPARE(QJsonDocument::fromJson(original).object()["schemaVersion"].toInt(), 6);
    CadDocument doc;
    const FileResult r = DocumentFile::load(path, doc);
    QVERIFY2(r.ok, qPrintable(r.message));
    QCOMPARE(doc.operations().size(), size_t(5));
    for (const auto& op : doc.operations())
        if (op.type == OperationType::Extrude)
            QVERIFY(op.extrudeMode == ExtrudeMode::NewBody && op.endCondition == EndCondition::Blind);
    QVERIFY(doc.operations()[4].chamferMode == Chamfer3DMode::Equal);
    QVERIFY(qAbs(ShapeInspector::volume(doc.shape()) - 69954.40) < 0.01);
    QCOMPARE(readFile(path), original); // documento antigo não é alterado
}

void TestArquivoCadmec::rejeitaE56Invalido_data()
{
    QTest::addColumn<QByteArray>("content");
    auto setOpField = [](QJsonObject& r, int i, const char* key, const QJsonValue& v) {
        QJsonObject o = opAt(r, i);
        if (v.isUndefined())
            o.remove(key);
        else
            o[key] = v;
        setOp(r, i, o);
    };
    auto editSupport = [](QJsonObject& r, std::function<void(QJsonObject&)> f) {
        QJsonObject s = sketchObject(r, 1);
        QJsonObject support = s["support"].toObject();
        f(support);
        s["support"] = support;
        setSketch(r, 1, s);
    };
    QTest::newRow("face no esquema 6") << e56Modified([](QJsonObject& r) {
        // Sem os campos de operação do esquema 7: o motivo é o plano "face".
        r["schemaVersion"] = 6;
        QJsonObject d = r["document"].toObject();
        QJsonArray ops = d["operations"].toArray();
        QJsonObject first = ops[0].toObject();
        first.remove("mode");
        first.remove("endCondition");
        QJsonArray ops1;
        ops1.append(first);
        d["operations"] = ops1;
        r["document"] = d;
        QJsonObject s0 = sketchObject(r, 0);
        QJsonArray ents = s0["entities"].toArray();
        ents.removeAt(1); // chanfro 2D com campos do esquema 7
        s0["entities"] = ents;
        setSketch(r, 0, s0); });
    QTest::newRow("face sem suporte") << e56Modified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 1); s.remove("support"); setSketch(r, 1, s); });
    QTest::newRow("suporte em plano XY") << e56Modified([](QJsonObject& r) {
        QJsonObject s = sketchObject(r, 0); s["support"] = sketchObject(r, 1)["support"]; setSketch(r, 0, s); });
    QTest::newRow("suporte campo desconhecido") << e56Modified([&](QJsonObject& r) { editSupport(r, [](QJsonObject& s) { s["index"] = 3; }); });
    QTest::newRow("suporte corpo inexistente") << e56Modified([&](QJsonObject& r) { editSupport(r, [](QJsonObject& s) { s["bodyId"] = "op-9"; }); });
    QTest::newRow("suporte corpo posterior") << e56Modified([&](QJsonObject& r) { editSupport(r, [](QJsonObject& s) { s["bodyId"] = "op-3"; }); });
    QTest::newRow("sistema local nao ortogonal") << e56Modified([&](QJsonObject& r) { editSupport(r, [](QJsonObject& s) {
        QJsonObject fr = s["frame"].toObject(); fr["xAxis"] = QJsonArray{0.0, 0.0, 1.0}; s["frame"] = fr; }); });
    QTest::newRow("face nao remapeavel") << e56Modified([&](QJsonObject& r) { editSupport(r, [](QJsonObject& s) {
        QJsonObject f = s["face"].toObject(); f["normal"] = QJsonArray{0.6, 0.0, 0.8}; s["face"] = f; }); });
    QTest::newRow("area da face invalida") << e56Modified([&](QJsonObject& r) { editSupport(r, [](QJsonObject& s) {
        QJsonObject f = s["face"].toObject(); f["area"] = -1.0; s["face"] = f; }); });
    QTest::newRow("modo de extrusao invalido") << e56Modified([&](QJsonObject& r) { setOpField(r, 1, "mode", "pocket"); });
    QTest::newRow("corte sem destino") << e56Modified([&](QJsonObject& r) { setOpField(r, 1, "targetId", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("novo corpo com destino") << e56Modified([&](QJsonObject& r) { setOpField(r, 0, "targetId", "op-1"); });
    QTest::newRow("condicao final invalida") << e56Modified([&](QJsonObject& r) { setOpField(r, 1, "endCondition", "upToFace"); });
    QTest::newRow("atravessar tudo na saliencia") << e56Modified([&](QJsonObject& r) { setOpField(r, 2, "endCondition", "throughAll"); });
    QTest::newRow("saliencia sem modo v7") << e56Modified([&](QJsonObject& r) { setOpField(r, 2, "mode", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("corpo de destino consumido") << e56Modified([&](QJsonObject& r) { setOpField(r, 2, "targetId", "op-1"); });
    QTest::newRow("chamfer2d modo invalido") << e56Modified([](QJsonObject& r) {
        QJsonObject e = entityAt(r, 0, 1); e["mode"] = "variable"; setEntity(r, 0, 1, e); });
    QTest::newRow("chamfer2d angulo inviavel") << e56Modified([](QJsonObject& r) {
        QJsonObject e = entityAt(r, 0, 1); e["angle"] = 120.0; setEntity(r, 0, 1, e); });
    QTest::newRow("chamfer2d sem flip") << e56Modified([](QJsonObject& r) {
        QJsonObject e = entityAt(r, 0, 1); e.remove("flip"); setEntity(r, 0, 1, e); });
    QTest::newRow("chamfer3d sem referencias") << e56Modified([&](QJsonObject& r) { setOpField(r, 3, "referenceNormals", QJsonValue(QJsonValue::Undefined)); });
    QTest::newRow("chamfer3d referencia errada") << e56Modified([&](QJsonObject& r) {
        // Normal válida que não é de nenhuma face adjacente à aresta.
        QJsonArray normals;
        normals.append(QJsonArray{1.0, 0.0, 0.0});
        setOpField(r, 3, "referenceNormals", normals); });
    QTest::newRow("chamfer3d sem distance2") << e56Modified([&](QJsonObject& r) {
        QJsonObject o = opAt(r, 3); o["parameters"] = QJsonObject{{"distance", 3.0}}; setOp(r, 3, o); });
    QTest::newRow("chamfer3d modo invalido") << e56Modified([&](QJsonObject& r) { setOpField(r, 3, "mode", "variable"); });
}

void TestArquivoCadmec::rejeitaE56Invalido()
{
    QFETCH(QByteArray, content);
    CadDocument active = documentE56();
    const TopoDS_Shape before = active.shape();
    const FileResult r = DocumentFile::fromJson(content, active);
    qInfo("%s -> %s", QTest::currentDataTag(), qPrintable(r.message));
    QVERIFY(!r.ok);
    QVERIFY(!r.message.isEmpty());
    QVERIFY(active.shape().IsSame(before));
    QCOMPARE(active.operations().size(), size_t(4));
}

QTEST_GUILESS_MAIN(TestArquivoCadmec)
#include "test_arquivo_cadmec.moc"
