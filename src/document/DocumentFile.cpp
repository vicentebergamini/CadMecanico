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

} // namespace

QByteArray DocumentFile::toJson(const CadDocument& document)
{
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

    // Limites geométricos e reconstrução (fromData só altera 'out' em caso de sucesso).
    std::string buildError;
    if (!CadDocument::fromData(name.toStdString(), p, ids[0].toStdString(), ids[1].toStdString(), out, buildError))
        return error(QStringLiteral("Parâmetros inválidos: %1").arg(QString::fromStdString(buildError)));
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
