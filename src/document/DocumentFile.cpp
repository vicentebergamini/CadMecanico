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
    case OperationType::Extrude:     return "extrude";
    case OperationType::Boolean:     return "boolean";
    case OperationType::Fillet3D:    return "fillet3d";
    case OperationType::Chamfer3D:   return "chamfer3d";
    }
    return "";
}

QJsonArray vec3(double x, double y, double z)
{
    return QJsonArray{x, y, z};
}

QJsonObject edgeToJson(const cad::modeling::EdgeSignature& s)
{
    QJsonArray normals;
    for (const gp_Dir& n : s.normals)
        normals.append(vec3(n.X(), n.Y(), n.Z()));
    return QJsonObject{
        {"curve", QString::fromLatin1(cad::modeling::curveName(s.curve))},
        {"mid", vec3(s.mid.X(), s.mid.Y(), s.mid.Z())},
        {"direction", vec3(s.direction.X(), s.direction.Y(), s.direction.Z())},
        {"length", s.length},
        {"radius", s.radius},
        {"normals", normals},
    };
}

bool readVec3(const QJsonValue& v, double out[3])
{
    if (!v.isArray() || v.toArray().size() != 3)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const QJsonValue c = v.toArray().at(i);
        if (!c.isDouble() || !std::isfinite(c.toDouble()))
            return false;
        out[i] = c.toDouble();
    }
    return true;
}

// Referência persistente de aresta (assinatura geométrica, ver EdgeNaming).
bool readEdge(const QJsonValue& value, cad::modeling::EdgeSignature& s, QString& message)
{
    if (!value.isObject())
    {
        message = QStringLiteral("referência de aresta não é um objeto");
        return false;
    }
    const QJsonObject o = value.toObject();
    const QStringList allowed{"curve", "mid", "direction", "length", "radius", "normals"};
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!allowed.contains(it.key()))
        {
            message = QStringLiteral("campo desconhecido \"%1\" na referência de aresta").arg(it.key());
            return false;
        }
    const QString curve = o.value("curve").toString();
    if (curve == QLatin1String("line"))
        s.curve = cad::modeling::EdgeSignature::Curve::Line;
    else if (curve == QLatin1String("circle"))
        s.curve = cad::modeling::EdgeSignature::Curve::Circle;
    else if (curve == QLatin1String("other"))
        s.curve = cad::modeling::EdgeSignature::Curve::Other;
    else
    {
        message = QStringLiteral("tipo de curva da aresta inválido");
        return false;
    }
    double mid[3], dir[3];
    if (!readVec3(o.value("mid"), mid) || !readVec3(o.value("direction"), dir))
    {
        message = QStringLiteral("ponto médio ou direção da aresta inválidos");
        return false;
    }
    if (std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) < 1e-9)
    {
        message = QStringLiteral("direção da aresta nula");
        return false;
    }
    for (const char* key : {"length", "radius"})
        if (!o.value(key).isDouble() || !std::isfinite(o.value(key).toDouble()) || o.value(key).toDouble() < 0.0)
        {
            message = QStringLiteral("campo \"%1\" da aresta inválido").arg(key);
            return false;
        }
    if (!o.value("normals").isArray() || o.value("normals").toArray().isEmpty() || o.value("normals").toArray().size() > 2)
    {
        message = QStringLiteral("normais das faces adjacentes inválidas");
        return false;
    }
    s.mid = gp_Pnt(mid[0], mid[1], mid[2]);
    s.direction = gp_Dir(dir[0], dir[1], dir[2]);
    s.length = o.value("length").toDouble();
    s.radius = o.value("radius").toDouble();
    s.normals.clear();
    for (const QJsonValue& n : o.value("normals").toArray())
    {
        double d[3];
        if (!readVec3(n, d) || std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < 1e-9)
        {
            message = QStringLiteral("normal de face inválida");
            return false;
        }
        s.normals.emplace_back(d[0], d[1], d[2]);
    }
    return true;
}

const char* booleanName(BooleanKind k)
{
    switch (k)
    {
    case BooleanKind::Fuse:   return "fuse";
    case BooleanKind::Cut:    return "cut";
    case BooleanKind::Common: return "common";
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

QJsonArray dir3(const gp_XYZ& v)
{
    return QJsonArray{v.X(), v.Y(), v.Z()};
}

bool readXYZ(const QJsonValue& v, gp_XYZ& out, bool unit)
{
    if (!v.isArray() || v.toArray().size() != 3)
        return false;
    double c[3];
    for (int i = 0; i < 3; ++i)
    {
        const QJsonValue x = v.toArray().at(i);
        if (!x.isDouble() || !std::isfinite(x.toDouble()))
            return false;
        c[i] = x.toDouble();
    }
    out.SetCoord(c[0], c[1], c[2]);
    return !unit || out.Modulus() > 1e-9;
}

// Suporte de esboço sobre face (esquema 7): corpo, assinatura da face e sistema local.
bool readSupport(const QJsonValue& value, cad::sketch::SketchSupport& support, cad::sketch::SketchPlane& plane,
                 QString& message)
{
    if (!value.isObject())
    {
        message = QStringLiteral("suporte da face ausente ou inválido");
        return false;
    }
    const QJsonObject o = value.toObject();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.key() != "bodyId" && it.key() != "face" && it.key() != "frame")
        {
            message = QStringLiteral("campo desconhecido \"%1\" no suporte").arg(it.key());
            return false;
        }
    if (!o.value("bodyId").isString() || !isValidIdentifier(o.value("bodyId").toString().toStdString()))
    {
        message = QStringLiteral("corpo de suporte inválido");
        return false;
    }
    support.bodyId = o.value("bodyId").toString().toStdString();
    const QJsonObject f = o.value("face").toObject();
    gp_XYZ normal, centroid, origin, xAxis, frameNormal;
    if (!o.value("face").isObject() || !readXYZ(f.value("normal"), normal, true) || !readXYZ(f.value("centroid"), centroid, false)
        || !f.value("offset").isDouble() || !std::isfinite(f.value("offset").toDouble()) || !f.value("area").isDouble()
        || !(f.value("area").toDouble() > 0.0) || f.size() != 4)
    {
        message = QStringLiteral("assinatura da face de suporte inválida");
        return false;
    }
    const QJsonObject fr = o.value("frame").toObject();
    if (!o.value("frame").isObject() || fr.size() != 3 || !readXYZ(fr.value("origin"), origin, false)
        || !readXYZ(fr.value("xAxis"), xAxis, true) || !readXYZ(fr.value("normal"), frameNormal, true)
        || std::fabs(gp_Dir(xAxis).Dot(gp_Dir(frameNormal))) > 1e-6)
    {
        message = QStringLiteral("sistema local do esboço inválido");
        return false;
    }
    support.face.normal = gp_Dir(normal);
    support.face.centroid = gp_Pnt(centroid);
    support.face.offset = f.value("offset").toDouble();
    support.face.area = f.value("area").toDouble();
    plane = cad::sketch::SketchPlane::fromFrame(gp_Pnt(origin), gp_Dir(frameNormal), gp_Dir(xAxis));
    return true;
}

// Lê um esboço (esquema 2). Chaves desconhecidas são recusadas.
bool readSketch(const QJsonValue& value, int index, int schema, cad::sketch::Sketch& out, QString& message)
{
    using namespace cad::sketch;
    const QString where = QStringLiteral("Esboço %1").arg(index + 1);
    if (!value.isObject())
    {
        message = where + QStringLiteral(": não é um objeto");
        return false;
    }
    const QJsonObject o = value.toObject();
    const QStringList allowed{"id", "name", "plane", "nextEntityNumber", "entities", "support"};
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
    if (!o.value("plane").isString() || !plane || (*plane == PlaneType::Face && schema < 7))
    {
        message = where + (schema >= 7 ? QStringLiteral(": plano inválido (esperado XY, XZ, YZ ou face)")
                                       : QStringLiteral(": plano inválido (esperado XY, XZ ou YZ)"));
        return false;
    }
    // Esboço sobre face (esquema 7): suporte obrigatório; nos planos padrão, proibido.
    std::optional<SketchSupport> support;
    SketchPlane facePlane;
    if (*plane == PlaneType::Face)
    {
        SketchSupport s;
        QString m;
        if (!readSupport(o.value("support"), s, facePlane, m))
        {
            message = where + QStringLiteral(": ") + m;
            return false;
        }
        support = s;
    }
    else if (o.contains("support"))
    {
        message = where + QStringLiteral(": \"support\" só se aplica a esboços sobre face");
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
        const bool e55 = *type == EntityType::Arc || *type == EntityType::Fillet || *type == EntityType::Chamfer;
        if (e55 && schema < 6)
        {
            message = ewhere + QStringLiteral(": tipo \"%1\" não pertence ao esquema %2")
                                   .arg(e.value("type").toString())
                                   .arg(schema);
            return false;
        }
        // Campos numéricos, de texto (referências) e lógicos de cada tipo.
        QStringList numbers, texts, flags;
        switch (*type)
        {
        case EntityType::Line:      numbers = {"x1", "y1", "x2", "y2"}; break;
        case EntityType::Rectangle: numbers = {"x", "y", "width", "height"}; break;
        case EntityType::Circle:    numbers = {"cx", "cy", "radius"}; break;
        case EntityType::Arc:
            numbers = {"cx", "cy", "radius", "startAngle", "endAngle"};
            flags = {"clockwise"};
            break;
        case EntityType::Fillet:
        case EntityType::Chamfer:
            numbers = {"corner", *type == EntityType::Fillet ? "radius" : "distance"};
            texts = {"entityA", "entityB"};
            if (*type == EntityType::Chamfer && schema >= 7)
            {
                numbers << "distance2" << "angle";
                texts << "mode";
                flags << "flip";
            }
            break;
        }
        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.key() != "id" && it.key() != "type" && !numbers.contains(it.key()) && !texts.contains(it.key())
                && !flags.contains(it.key()))
            {
                message = ewhere + QStringLiteral(": campo desconhecido \"%1\"").arg(it.key());
                return false;
            }
        double v[6] = {0, 0, 0, 0, 0, 0};
        for (int k = 0; k < numbers.size(); ++k)
        {
            const QJsonValue x = e.value(numbers[k]);
            if (!x.isDouble() || !std::isfinite(x.toDouble()))
            {
                message = ewhere + QStringLiteral(": campo \"%1\" ausente ou não numérico").arg(numbers[k]);
                return false;
            }
            v[k] = x.toDouble();
        }
        std::string refs[3];
        for (int k = 0; k < texts.size(); ++k)
        {
            if (!e.value(texts[k]).isString())
            {
                message = ewhere + QStringLiteral(": referência \"%1\" ausente ou inválida").arg(texts[k]);
                return false;
            }
            refs[k] = e.value(texts[k]).toString().toStdString();
        }
        for (const QString& f : flags)
            if (!e.value(f).isBool())
            {
                message = ewhere + QStringLiteral(": campo \"%1\" ausente ou inválido").arg(f);
                return false;
            }
        if (!texts.isEmpty() && v[0] != std::floor(v[0]))
        {
            message = ewhere + QStringLiteral(": \"corner\" deve ser inteiro");
            return false;
        }
        SketchEntity entity;
        entity.id = e.value("id").toString().toStdString();
        switch (*type)
        {
        case EntityType::Line:      entity.geometry = SketchLine{v[0], v[1], v[2], v[3]}; break;
        case EntityType::Rectangle: entity.geometry = SketchRectangle{v[0], v[1], v[2], v[3]}; break;
        case EntityType::Circle:    entity.geometry = SketchCircle{v[0], v[1], v[2]}; break;
        case EntityType::Arc:
            entity.geometry = SketchArc{v[0], v[1], v[2], v[3], v[4], e.value("clockwise").toBool()};
            break;
        case EntityType::Fillet:    entity.geometry = SketchFillet{refs[0], refs[1], int(v[0]), v[1]}; break;
        case EntityType::Chamfer:
        {
            SketchChamfer c{refs[0], refs[1], int(v[0]), v[1]};
            if (schema >= 7)
            {
                if (refs[2] == chamferModeName(ChamferMode::Equal))
                    c.mode = ChamferMode::Equal;
                else if (refs[2] == chamferModeName(ChamferMode::TwoDistances))
                    c.mode = ChamferMode::TwoDistances;
                else if (refs[2] == chamferModeName(ChamferMode::DistanceAngle))
                    c.mode = ChamferMode::DistanceAngle;
                else
                {
                    message = ewhere + QStringLiteral(": modo de chanfro inválido");
                    return false;
                }
                c.distance2 = v[2];
                c.angle = v[3];
                c.flip = e.value("flip").toBool();
            }
            entity.geometry = c;
            break;
        }
        }
        entities.push_back(entity);
    }

    std::string error;
    if (!Sketch::fromData(o.value("id").toString().toStdString(), name.toString().trimmed().toStdString(), *plane,
                          entities, nextEntity, out, error))
    {
        message = where + QStringLiteral(": ") + QString::fromStdString(error);
        return false;
    }
    if (support)
    {
        out.setPlane(facePlane);
        out.setSupport(support);
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
        else if (const auto* c = std::get_if<SketchCircle>(&e.geometry))
        {
            o.insert("cx", c->cx);
            o.insert("cy", c->cy);
            o.insert("radius", c->radius);
        }
        else if (const auto* a = std::get_if<SketchArc>(&e.geometry))
        {
            o.insert("cx", a->cx);
            o.insert("cy", a->cy);
            o.insert("radius", a->radius);
            o.insert("startAngle", a->startAngle);
            o.insert("endAngle", a->endAngle);
            o.insert("clockwise", a->clockwise);
        }
        else if (const auto* f = std::get_if<SketchFillet>(&e.geometry))
        {
            o.insert("entityA", QString::fromStdString(f->entityA));
            o.insert("entityB", QString::fromStdString(f->entityB));
            o.insert("corner", f->corner);
            o.insert("radius", f->radius);
        }
        else
        {
            const auto& c = std::get<SketchChamfer>(e.geometry);
            o.insert("entityA", QString::fromStdString(c.entityA));
            o.insert("entityB", QString::fromStdString(c.entityB));
            o.insert("corner", c.corner);
            o.insert("distance", c.distance);
            o.insert("mode", QString::fromLatin1(chamferModeName(c.mode)));
            o.insert("distance2", c.distance2);
            o.insert("angle", c.angle);
            o.insert("flip", c.flip);
        }
        entities.append(o);
    }
    QJsonObject result{
        {"id", QString::fromStdString(sketch.id())},
        {"name", QString::fromStdString(sketch.name())},
        {"plane", QString::fromLatin1(SketchPlane::name(sketch.plane().type()))},
        {"nextEntityNumber", sketch.nextEntityNumber()},
        {"entities", entities},
    };
    if (const auto& s = sketch.support())
    {
        const SketchPlane& p = sketch.plane();
        result.insert("support",
                      QJsonObject{{"bodyId", QString::fromStdString(s->bodyId)},
                                  {"face", QJsonObject{{"normal", dir3(s->face.normal.XYZ())},
                                                       {"offset", s->face.offset},
                                                       {"centroid", dir3(s->face.centroid.XYZ())},
                                                       {"area", s->face.area}}},
                                  {"frame", QJsonObject{{"origin", dir3(p.origin().XYZ())},
                                                        {"xAxis", dir3(p.horizontalAxis().XYZ())},
                                                        {"normal", dir3(p.normal().XYZ())}}}});
    }
    return result;
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
        QJsonObject o{
            {"id", QString::fromStdString(op.id)},
            {"type", QString::fromLatin1(typeName(op.type))},
            {"name", QString::fromStdString(op.name)},
        };
        if (op.type == OperationType::Boolean)
        {
            o.insert("operation", QString::fromLatin1(booleanName(op.booleanKind)));
            o.insert("operandA", QString::fromStdString(op.operandA));
            o.insert("operandB", QString::fromStdString(op.operandB));
        }
        else
        {
            o.insert("parameters", params);
        }
        if (op.type == OperationType::Fillet3D || op.type == OperationType::Chamfer3D)
        {
            o.insert("sourceId", QString::fromStdString(op.sourceId));
            QJsonArray edges;
            for (const auto& e : op.edges)
                edges.append(edgeToJson(e));
            o.insert("edges", edges);
            if (op.type == OperationType::Chamfer3D)
            {
                o.insert("mode", QString::fromLatin1(op.chamferMode == Chamfer3DMode::TwoDistances    ? "twoDistances"
                                                     : op.chamferMode == Chamfer3DMode::DistanceAngle ? "distanceAngle"
                                                                                                      : "equal"));
                if (op.chamferMode != Chamfer3DMode::Equal)
                {
                    QJsonArray normals;
                    for (const gp_Dir& n : op.referenceNormals)
                        normals.append(vec3(n.X(), n.Y(), n.Z()));
                    o.insert("referenceNormals", normals);
                }
            }
        }
        if (op.type == OperationType::Extrude)
        {
            o.insert("sketchId", QString::fromStdString(op.sketchId));
            o.insert("reversed", op.reversed);
            o.insert("mode", QString::fromLatin1(op.extrudeMode == ExtrudeMode::Boss  ? "boss"
                                                 : op.extrudeMode == ExtrudeMode::Cut ? "cut"
                                                                                      : "newBody"));
            if (op.extrudeMode != ExtrudeMode::NewBody)
                o.insert("targetId", QString::fromStdString(op.targetId));
            o.insert("endCondition", QString::fromLatin1(op.endCondition == EndCondition::ThroughAll ? "throughAll" : "blind"));
        }
        operations.append(o);
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
             {"nextOperationNumber", document.nextOperationNumber()},
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
    CadDocument::Data data;
    data.name = name.toStdString();

    // Esquemas 1 e 2: exatamente bloco base + furo passante. Esquema 3: também lista vazia.
    // Esquema 4: par bloco + furo opcional no início, seguido de extrusões (5: e booleanas).
    int index = 0;
    const bool startsWithPart = !ops.isEmpty() && ops.at(0).toObject().value("type").toString() == QLatin1String("block");
    if (schema <= 3)
    {
        const bool emptyAllowed = schema == 3;
        if (ops.size() != 2 && !(emptyAllowed && ops.isEmpty()))
            return error(emptyAllowed
                             ? QStringLiteral("Sequência de operações não suportada (esperado nenhuma operação, ou bloco base e furo passante)")
                             : QStringLiteral("Sequência de operações não suportada (esperado bloco base e furo passante)"));
    }
    if (schema <= 3 ? !ops.isEmpty() : startsWithPart)
    {
        if (ops.size() < 2)
            return error(QStringLiteral("Bloco base sem furo passante"));
        PartParameters p;
        QString ids[2];
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
        if (ids[0] == ids[1])
            return error(QStringLiteral("Identificador de operação duplicado: %1").arg(ids[0]));
        data.part = p;
        data.blockId = ids[0].toStdString();
        data.holeId = ids[1].toStdString();
        index = 2;
    }
    for (int i = index; i < ops.size(); ++i)
    {
        if (!ops.at(i).isObject())
            return error(QStringLiteral("Operação %1 não é um objeto").arg(i + 1));
        const QJsonObject op = ops.at(i).toObject();
        const QString type = op.value("type").toString();
        const bool isBoolean = type == QLatin1String("boolean") && schema >= 5;
        const bool isFinish = (type == QLatin1String("fillet3d") || type == QLatin1String("chamfer3d")) && schema >= 6;
        if (type != QLatin1String("extrude") && !isBoolean && !isFinish)
            return error(QStringLiteral("Operação %1: tipo inesperado \"%2\" (esperado %3)")
                             .arg(i + 1)
                             .arg(type, schema >= 6   ? QStringLiteral("\"extrude\", \"boolean\", \"fillet3d\" ou \"chamfer3d\"")
                                        : schema >= 5 ? QStringLiteral("\"extrude\" ou \"boolean\"")
                                                      : QStringLiteral("\"extrude\"")));
        QStringList allowed = isBoolean  ? QStringList{"id", "type", "name", "operation", "operandA", "operandB"}
                              : isFinish ? QStringList{"id", "type", "name", "sourceId", "parameters", "edges"}
                                         : QStringList{"id", "type", "name", "sketchId", "reversed", "parameters"};
        if (schema >= 7 && !isBoolean)
        {
            // Esquema 7: modo/destino/condição final da extrusão; modo e referências do Chamfer 3D.
            if (isFinish && type == QLatin1String("chamfer3d"))
                allowed << "mode" << "referenceNormals";
            else if (!isFinish)
                allowed << "mode" << "targetId" << "endCondition";
        }
        for (auto it = op.begin(); it != op.end(); ++it)
            if (!allowed.contains(it.key()))
                return error(QStringLiteral("Operação %1: campo desconhecido \"%2\"").arg(i + 1).arg(it.key()));
        const QJsonValue id = op.value("id");
        if (!id.isString() || !isValidIdentifier(id.toString().toStdString()))
            return error(QStringLiteral("Operação %1: identificador inválido").arg(i + 1));
        if (!op.value("name").isString())
            return error(QStringLiteral("Operação %1: nome ausente ou inválido").arg(i + 1));
        if (isBoolean)
        {
            Operation b;
            b.id = id.toString().toStdString();
            b.type = OperationType::Boolean;
            b.name = op.value("name").toString().toStdString();
            const QString kind = op.value("operation").toString();
            if (kind == QLatin1String("fuse"))
                b.booleanKind = BooleanKind::Fuse;
            else if (kind == QLatin1String("cut"))
                b.booleanKind = BooleanKind::Cut;
            else if (kind == QLatin1String("common"))
                b.booleanKind = BooleanKind::Common;
            else
                return error(QStringLiteral("Operação %1: campo \"operation\" inválido (esperado fuse, cut ou common)")
                                 .arg(i + 1));
            for (const char* key : {"operandA", "operandB"})
                if (!op.value(key).isString() || !isValidIdentifier(op.value(key).toString().toStdString()))
                    return error(QStringLiteral("Operação %1: referência \"%2\" ausente ou inválida").arg(i + 1).arg(key));
            b.operandA = op.value("operandA").toString().toStdString();
            b.operandB = op.value("operandB").toString().toStdString();
            data.features.push_back(b);
            continue;
        }
        if (isFinish)
        {
            Operation f;
            f.id = id.toString().toStdString();
            f.type = type == QLatin1String("fillet3d") ? OperationType::Fillet3D : OperationType::Chamfer3D;
            f.name = op.value("name").toString().toStdString();
            if (!op.value("sourceId").isString() || !isValidIdentifier(op.value("sourceId").toString().toStdString()))
                return error(QStringLiteral("Operação %1: referência \"sourceId\" ausente ou inválida").arg(i + 1));
            f.sourceId = op.value("sourceId").toString().toStdString();
            const char* key = f.type == OperationType::Fillet3D ? "radius" : "distance";
            QStringList keys{QString::fromLatin1(key)};
            if (f.type == OperationType::Chamfer3D && schema >= 7)
            {
                const QString mode = op.value("mode").toString();
                if (mode == QLatin1String("equal"))
                    f.chamferMode = Chamfer3DMode::Equal;
                else if (mode == QLatin1String("twoDistances"))
                    f.chamferMode = Chamfer3DMode::TwoDistances;
                else if (mode == QLatin1String("distanceAngle"))
                    f.chamferMode = Chamfer3DMode::DistanceAngle;
                else
                    return error(QStringLiteral("Operação %1: campo \"mode\" do chanfro inválido").arg(i + 1));
                if (f.chamferMode == Chamfer3DMode::TwoDistances)
                    keys << "distance2";
                if (f.chamferMode == Chamfer3DMode::DistanceAngle)
                    keys << "angle";
                if (f.chamferMode != Chamfer3DMode::Equal)
                {
                    if (!op.value("referenceNormals").isArray())
                        return error(QStringLiteral("Operação %1: lista \"referenceNormals\" ausente").arg(i + 1));
                    for (const QJsonValue& n : op.value("referenceNormals").toArray())
                    {
                        gp_XYZ d;
                        if (!readXYZ(n, d, true))
                            return error(QStringLiteral("Operação %1: normal de referência inválida").arg(i + 1));
                        f.referenceNormals.emplace_back(d);
                    }
                }
                else if (op.contains("referenceNormals"))
                    return error(QStringLiteral("Operação %1: \"referenceNormals\" só se aplica a chanfros assimétricos").arg(i + 1));
            }
            QList<double> values;
            QString message;
            if (!readParameters(op, keys, values, message))
                return error(QStringLiteral("Operação %1: %2").arg(i + 1).arg(message));
            f.parameters.clear();
            for (int k = 0; k < keys.size(); ++k)
                f.parameters.push_back({keys[k].toStdString(), values[k]});
            if (!op.value("edges").isArray() || op.value("edges").toArray().isEmpty())
                return error(QStringLiteral("Operação %1: lista \"edges\" ausente ou vazia").arg(i + 1));
            for (const QJsonValue& ev : op.value("edges").toArray())
            {
                cad::modeling::EdgeSignature s;
                if (!readEdge(ev, s, message))
                    return error(QStringLiteral("Operação %1: %2").arg(i + 1).arg(message));
                f.edges.push_back(s);
            }
            data.features.push_back(f);
            continue;
        }
        if (!op.value("sketchId").isString())
            return error(QStringLiteral("Operação %1: referência ao esboço ausente ou inválida").arg(i + 1));
        if (!op.value("reversed").isBool())
            return error(QStringLiteral("Operação %1: campo \"reversed\" ausente ou inválido").arg(i + 1));
        QList<double> values;
        QString message;
        if (!readParameters(op, QStringList{"depth"}, values, message))
            return error(QStringLiteral("Operação %1: %2").arg(i + 1).arg(message));
        Operation e;
        if (schema >= 7)
        {
            const QString mode = op.value("mode").toString();
            if (mode == QLatin1String("newBody"))
                e.extrudeMode = ExtrudeMode::NewBody;
            else if (mode == QLatin1String("boss"))
                e.extrudeMode = ExtrudeMode::Boss;
            else if (mode == QLatin1String("cut"))
                e.extrudeMode = ExtrudeMode::Cut;
            else
                return error(QStringLiteral("Operação %1: campo \"mode\" da extrusão inválido (newBody, boss ou cut)").arg(i + 1));
            const QString end = op.value("endCondition").toString();
            if (end == QLatin1String("blind"))
                e.endCondition = EndCondition::Blind;
            else if (end == QLatin1String("throughAll"))
                e.endCondition = EndCondition::ThroughAll;
            else
                return error(QStringLiteral("Operação %1: campo \"endCondition\" inválido (blind ou throughAll)").arg(i + 1));
            if (e.extrudeMode == ExtrudeMode::NewBody)
            {
                if (op.contains("targetId"))
                    return error(QStringLiteral("Operação %1: \"targetId\" não se aplica a novo corpo").arg(i + 1));
            }
            else if (!op.value("targetId").isString() || !isValidIdentifier(op.value("targetId").toString().toStdString()))
                return error(QStringLiteral("Operação %1: corpo de destino ausente ou inválido").arg(i + 1));
            e.targetId = op.value("targetId").toString().toStdString();
        }
        e.id = id.toString().toStdString();
        e.type = OperationType::Extrude;
        e.name = op.value("name").toString().toStdString();
        e.sketchId = op.value("sketchId").toString().toStdString();
        e.reversed = op.value("reversed").toBool();
        e.parameters = {{"depth", values[0]}};
        data.features.push_back(e);
    }

    // Contador de operações: a partir do esquema 4.
    if (schema >= 4)
    {
        if (!readInt(doc, "nextOperationNumber", 1, data.nextOperationNumber))
            return error(QStringLiteral("Campo \"nextOperationNumber\" ausente ou inválido"));
    }
    else if (doc.contains("nextOperationNumber"))
    {
        return error(QStringLiteral("Campo \"nextOperationNumber\" não pertence ao esquema %1").arg(schema));
    }
    // Esboços: inexistentes na versão 1; obrigatórios (lista, possivelmente vazia) na versão 2.
    std::vector<cad::sketch::Sketch>& sketches = data.sketches;
    int& nextSketchNumber = data.nextSketchNumber;
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
            if (!readSketch(array.at(i), i, schema, sketch, message))
                return error(message);
            sketches.push_back(sketch);
        }
    }

    // Limites geométricos e reconstrução (fromData só altera 'out' em caso de sucesso).
    std::string buildError;
    if (!CadDocument::fromData(data, out, buildError))
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
