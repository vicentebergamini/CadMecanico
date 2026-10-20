#include "document/DocumentFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <cmath>

namespace cad::document {

namespace {

constexpr int kMaxNameLength = 200;

const char* typeName(OperationType t)
{
    switch (t)
    {
    case OperationType::Block:       return "block";
    case OperationType::ThroughHole: return "throughHole";
    }
    return "";
}

FileResult error(const QString& message)
{
    return FileResult{false, message};
}

// Lê os parâmetros numéricos exigidos; rejeita ausentes, não numéricos e desconhecidos.
bool readParameters(const QJsonObject& op, const QStringList& keys, QList<double>& values, QString& message)
{
    const QJsonValue pv = op.value("parameters");
    if (!pv.isObject())
    {
        message = QStringLiteral("operação sem objeto \"parameters\"");
        return false;
    }
    const QJsonObject params = pv.toObject();
    for (auto it = params.begin(); it != params.end(); ++it)
        if (!keys.contains(it.key()))
        {
            message = QStringLiteral("parâmetro desconhecido \"%1\"").arg(it.key());
            return false;
        }
    values.clear();
    for (const QString& key : keys)
    {
        const QJsonValue v = params.value(key);
        if (!v.isDouble() || !std::isfinite(v.toDouble()))
        {
            message = QStringLiteral("parâmetro \"%1\" ausente ou não numérico").arg(key);
            return false;
        }
        values << v.toDouble();
    }
    return true;
}

bool readInt(const QJsonObject& o, const char* key, int minimum, int& value)
{
    const QJsonValue v = o.value(key);
    if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()) || v.toDouble() < minimum || v.toDouble() > 1e9)
        return false;
    value = v.toInt();
    return true;
}

// Lê um esboço (esquema 2). Chaves desconhecidas são recusadas.
bool readSketch(const QJsonValue& value, int index, cad::sketch::Sketch& out, QString& message)
{
    using namespace cad::sketch;
    const QString where = QStringLiteral("Esboço %1").arg(index + 1);
    if (!value.isObject())
    {
        message = where + QStringLiteral(": não é um objeto");
        return false;
    }
    const QJsonObject o = value.toObject();
    const QStringList allowed{"id", "name", "plane", "nextEntityNumber", "entities"};
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!allowed.contains(it.key()))
        {
            message = where + QStringLiteral(": campo desconhecido \"%1\"").arg(it.key());
            return false;
        }
    if (!o.value("id").isString())
    {
        message = where + QStringLiteral(": identificador inválido");
        return false;
    }
    const QJsonValue name = o.value("name");
    if (!name.isString() || name.toString().trimmed().isEmpty() || name.toString().size() > kMaxNameLength)
    {
        message = where + QStringLiteral(": nome inválido");
        return false;
    }
    const auto plane = SketchPlane::fromName(o.value("plane").toString().toStdString());
    if (!o.value("plane").isString() || !plane)
    {
        message = where + QStringLiteral(": plano inválido (esperado XY, XZ ou YZ)");
        return false;
    }
    int nextEntity = 0;
    if (!readInt(o, "nextEntityNumber", 1, nextEntity))
    {
        message = where + QStringLiteral(": \"nextEntityNumber\" inválido");
        return false;
    }
    if (!o.value("entities").isArray())
    {
        message = where + QStringLiteral(": lista \"entities\" ausente");
        return false;
    }

    std::vector<SketchEntity> entities;
    const QJsonArray array = o.value("entities").toArray();
    for (int i = 0; i < array.size(); ++i)
    {
        const QString ewhere = where + QStringLiteral(", entidade %1").arg(i + 1);
        if (!array.at(i).isObject())
        {
            message = ewhere + QStringLiteral(": não é um objeto");
            return false;
        }
        const QJsonObject e = array.at(i).toObject();
        if (!e.value("id").isString() || !e.value("type").isString())
        {
            message = ewhere + QStringLiteral(": identificador ou tipo ausente");
            return false;
        }
        const auto type = typeFromName(e.value("type").toString().toStdString());
        if (!type)
        {
            message = ewhere + QStringLiteral(": tipo desconhecido \"%1\"").arg(e.value("type").toString());
            return false;
        }
        const QStringList keys = *type == EntityType::Line        ? QStringList{"x1", "y1", "x2", "y2"}
                                 : *type == EntityType::Rectangle ? QStringList{"x", "y", "width", "height"}
                                                                  : QStringList{"cx", "cy", "radius"};
        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.key() != "id" && it.key() != "type" && !keys.contains(it.key()))
            {
                message = ewhere + QStringLiteral(": campo desconhecido \"%1\"").arg(it.key());
                return false;
            }
        double v[4] = {0, 0, 0, 0};
        for (int k = 0; k < keys.size(); ++k)
        {
            const QJsonValue x = e.value(keys[k]);
            if (!x.isDouble() || !std::isfinite(x.toDouble()))
            {
                message = ewhere + QStringLiteral(": campo \"%1\" ausente ou não numérico").arg(keys[k]);
                return false;
            }
            v[k] = x.toDouble();
        }
        SketchEntity entity;
        entity.id = e.value("id").toString().toStdString();
        if (*type == EntityType::Line)
            entity.geometry = SketchLine{v[0], v[1], v[2], v[3]};
        else if (*type == EntityType::Rectangle)
            entity.geometry = SketchRectangle{v[0], v[1], v[2], v[3]};
        else
            entity.geometry = SketchCircle{v[0], v[1], v[2]};
        entities.push_back(entity);
    }

    std::string error;
    if (!Sketch::fromData(o.value("id").toString().toStdString(), name.toString().trimmed().toStdString(), *plane,
                          entities, nextEntity, out, error))
    {
        message = where + QStringLiteral(": ") + QString::fromStdString(error);
        return false;
    }
    return true;
}

QJsonObject sketchToJson(const cad::sketch::Sketch& sketch)
{
    using namespace cad::sketch;
    QJsonArray entities;
    for (const SketchEntity& e : sketch.entities())
    {
        QJsonObject o{{"id", QString::fromStdString(e.id)}, {"type", QString::fromLatin1(typeName(e.type()))}};
        if (const auto* l = std::get_if<SketchLine>(&e.geometry))
        {
            o.insert("x1", l->x1);
            o.insert("y1", l->y1);
            o.insert("x2", l->x2);
            o.insert("y2", l->y2);
        }
        else if (const auto* r = std::get_if<SketchRectangle>(&e.geometry))
        {
            o.insert("x", r->x);
            o.insert("y", r->y);
            o.insert("width", r->width);
            o.insert("height", r->height);
        }
        else
        {
            const auto& c = std::get<SketchCircle>(e.geometry);
            o.insert("cx", c.cx);
            o.insert("cy", c.cy);
            o.insert("radius", c.radius);
        }
        entities.append(o);
    }
    return QJsonObject{
        {"id", QString::fromStdString(sketch.id())},
        {"name", QString::fromStdString(sketch.name())},
        {"plane", QString::fromLatin1(SketchPlane::name(sketch.plane().type()))},
        {"nextEntityNumber", sketch.nextEntityNumber()},
        {"entities", entities},
    };
}

} // namespace

QByteArray DocumentFile::toJson(const CadDocument& document)
{
    QJsonArray sketches;
    for (const auto& s : document.sketches())
        sketches.append(sketchToJson(s));

    QJsonArray operations;
    for (const Operation& op : document.operations())
    {
        QJsonObject params;
        for (const OperationParameter& p : op.parameters)
            params.insert(QString::fromStdString(p.key), p.value);
        operations.append(QJsonObject{
            {"id", QString::fromStdString(op.id)},
            {"type", QString::fromLatin1(typeName(op.type))},
            {"name", QString::fromStdString(op.name)},
            {"parameters", params},
        });
    }

    const QJsonObject root{
        {"format", QString::fromLatin1(kFormat)},
        {"schemaVersion", kSchemaVersion},
        {"generator", QStringLiteral("CadMecanico")},
        {"document",
         QJsonObject{
             {"name", QString::fromStdString(document.name())},
             {"unit", QString::fromLatin1(document.unit())},
             {"operations", operations},
             {"nextSketchNumber", document.nextSketchNumber()},
             {"sketches", sketches},
         }},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

FileResult DocumentFile::fromJson(const QByteArray& json, CadDocument& out)
{
    QJsonParseError parseError;
    const QJsonDocument jd = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return error(QStringLiteral("JSON inválido (posição %1): %2").arg(parseError.offset).arg(parseError.errorString()));
    if (!jd.isObject())
        return error(QStringLiteral("Raiz do arquivo deve ser um objeto JSON"));

    const QJsonObject root = jd.object();
    if (root.value("format").toString() != QLatin1String(kFormat))
        return error(QStringLiteral("Arquivo não é um documento CadMecanico (campo \"format\")"));

    const QJsonValue version = root.value("schemaVersion");
    if (!version.isDouble() || version.toDouble() != std::floor(version.toDouble()))
        return error(QStringLiteral("Campo \"schemaVersion\" ausente ou inválido"));
    const int schema = version.toInt();
    if (schema < 1)
        return error(QStringLiteral("Versão de esquema inválida: %1").arg(schema));
    if (schema > kSchemaVersion)
        return error(QStringLiteral("Versão de esquema %1 não suportada (suportada até %2)").arg(schema).arg(kSchemaVersion));

    const QJsonValue docValue = root.value("document");
    if (!docValue.isObject())
        return error(QStringLiteral("Objeto \"document\" ausente"));
    const QJsonObject doc = docValue.toObject();

    const QJsonValue nameValue = doc.value("name");
    if (!nameValue.isString())
        return error(QStringLiteral("Nome do documento ausente ou inválido"));
    const QString name = nameValue.toString().trimmed();
    if (name.isEmpty() || name.size() > kMaxNameLength)
        return error(QStringLiteral("Nome do documento vazio ou longo demais"));

    if (doc.value("unit").toString() != QLatin1String(kLengthUnit))
        return error(QStringLiteral("Unidade não suportada (esperado \"mm\")"));

    const QJsonValue opsValue = doc.value("operations");
    if (!opsValue.isArray())
        return error(QStringLiteral("Lista \"operations\" ausente"));
    const QJsonArray ops = opsValue.toArray();
    // Sequência suportada no esquema 1: bloco base seguido de furo passante.
    if (ops.size() != 2)
        return error(QStringLiteral("Sequência de operações não suportada (esperado bloco base e furo passante)"));

    PartParameters p;
    QString ids[2];
    QSet<QString> seen;
    const char* expectedTypes[2] = {"block", "throughHole"};
    for (int i = 0; i < 2; ++i)
    {
        if (!ops.at(i).isObject())
            return error(QStringLiteral("Operação %1 não é um objeto").arg(i + 1));
        const QJsonObject op = ops.at(i).toObject();
        if (op.value("type").toString() != QLatin1String(expectedTypes[i]))
            return error(QStringLiteral("Operação %1: tipo inesperado (esperado \"%2\")").arg(i + 1).arg(expectedTypes[i]));
        const QJsonValue id = op.value("id");
        if (!id.isString() || !isValidIdentifier(id.toString().toStdString()))
            return error(QStringLiteral("Operação %1: identificador inválido").arg(i + 1));
        if (seen.contains(id.toString()))
            return error(QStringLiteral("Identificador de operação duplicado: %1").arg(id.toString()));
        seen.insert(id.toString());
        ids[i] = id.toString();
        if (op.contains("name") && !op.value("name").isString())
            return error(QStringLiteral("Operação %1: nome inválido").arg(i + 1));

        QList<double> values;
        QString message;
        const QStringList keys = i == 0 ? QStringList{"length", "width", "height"} : QStringList{"radius"};
        if (!readParameters(op, keys, values, message))
            return error(QStringLiteral("Operação %1: %2").arg(i + 1).arg(message));
        if (i == 0)
        {
            p.length = values[0];
            p.width = values[1];
            p.height = values[2];
        }
        else
        {
            p.holeRadius = values[0];
        }
    }

    // Esboços: inexistentes na versão 1; obrigatórios (lista, possivelmente vazia) na versão 2.
    std::vector<cad::sketch::Sketch> sketches;
    int nextSketchNumber = 1;
    if (schema == 1)
    {
        if (doc.contains("sketches") || doc.contains("nextSketchNumber"))
            return error(QStringLiteral("Campos de esboço não pertencem ao esquema 1"));
    }
    else
    {
        if (!doc.value("sketches").isArray())
            return error(QStringLiteral("Lista \"sketches\" ausente"));
        if (!readInt(doc, "nextSketchNumber", 1, nextSketchNumber))
            return error(QStringLiteral("Campo \"nextSketchNumber\" ausente ou inválido"));
        const QJsonArray array = doc.value("sketches").toArray();
        for (int i = 0; i < array.size(); ++i)
        {
            cad::sketch::Sketch sketch;
            QString message;
            if (!readSketch(array.at(i), i, sketch, message))
                return error(message);
            sketches.push_back(sketch);
        }
    }

    // Limites geométricos e reconstrução (fromData só altera 'out' em caso de sucesso).
    std::string buildError;
    if (!CadDocument::fromData(name.toStdString(), p, ids[0].toStdString(), ids[1].toStdString(), out, buildError,
                               sketches, nextSketchNumber))
        return error(QStringLiteral("Dados inválidos: %1").arg(QString::fromStdString(buildError)));
    return FileResult{true, QString()};
}

FileResult DocumentFile::save(const CadDocument& document, const QString& path)
{
    const QFileInfo info(path);
    if (!info.absoluteDir().exists())
        return error(QStringLiteral("Diretório inexistente: %1").arg(info.absolutePath()));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return error(QStringLiteral("Não foi possível criar o arquivo: %1").arg(file.errorString()));
    const QByteArray data = toJson(document);
    if (file.write(data) != data.size())
    {
        file.cancelWriting();
        return error(QStringLiteral("Falha ao gravar: %1").arg(file.errorString()));
    }
    if (!file.commit())
        return error(QStringLiteral("Falha ao concluir a gravação: %1").arg(file.errorString()));
    return FileResult{true, QString()};
}

FileResult DocumentFile::load(const QString& path, CadDocument& out)
{
    QFile file(path);
    if (!file.exists())
        return error(QStringLiteral("Arquivo não encontrado"));
    if (file.size() > kMaxFileSize)
        return error(QStringLiteral("Arquivo grande demais para um documento CadMecanico"));
    if (!file.open(QIODevice::ReadOnly))
        return error(QStringLiteral("Não foi possível abrir o arquivo: %1").arg(file.errorString()));
    return fromJson(file.readAll(), out);
}

} // namespace cad::document
