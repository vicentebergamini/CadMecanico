#include "app/SketchEntityDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace cad::app {

using namespace cad::sketch;

namespace {

QString entityTitle(const SketchEntity& e)
{
    switch (e.type())
    {
    case EntityType::Line:      return SketchEntityDialog::tr("Linha %1").arg(QString::fromStdString(e.id));
    case EntityType::Rectangle: return SketchEntityDialog::tr("Retângulo %1").arg(QString::fromStdString(e.id));
    case EntityType::Circle:    return SketchEntityDialog::tr("Círculo %1").arg(QString::fromStdString(e.id));
    case EntityType::Arc:       return SketchEntityDialog::tr("Arco %1").arg(QString::fromStdString(e.id));
    case EntityType::Fillet:    return SketchEntityDialog::tr("Fillet 2D %1").arg(QString::fromStdString(e.id));
    case EntityType::Chamfer:   return SketchEntityDialog::tr("Chamfer 2D %1").arg(QString::fromStdString(e.id));
    }
    return QString();
}

bool isSizeKey(const std::string& key)
{
    return key == "width" || key == "height" || key == "radius" || key == "distance";
}

} // namespace

SketchEntityDialog::SketchEntityDialog(const SketchEntity& entity, QWidget* parent)
    : QDialog(parent)
    , m_entity(entity)
{
    setObjectName("dialogoDimensoesEsboco");
    setWindowTitle(tr("Dimensões — %1").arg(entityTitle(entity)));

    auto* form = new QFormLayout;
    for (const SketchDimension& d : dimensionsOf(entity))
    {
        const QString key = QString::fromStdString(d.key);
        const QString label = QString::fromStdString(d.label) + ':';
        if (d.editable && d.kind == DimensionKind::Choice)
        {
            auto* box = new QComboBox;
            box->setObjectName("dim_" + key);
            for (const std::string& c : d.choices)
                box->addItem(QString::fromStdString(c));
            box->setCurrentIndex(int(d.value));
            connect(box, &QComboBox::currentIndexChanged, this, &SketchEntityDialog::updateComputed);
            form->addRow(label, box);
            m_choices.insert(key, box);
        }
        else if (d.editable && d.kind == DimensionKind::Flag)
        {
            auto* box = new QCheckBox;
            box->setObjectName("dim_" + key);
            box->setChecked(d.value != 0.0);
            connect(box, &QCheckBox::toggled, this, &SketchEntityDialog::updateComputed);
            form->addRow(label, box);
            m_flags.insert(key, box);
        }
        else if (d.editable)
        {
            auto* field = new QDoubleSpinBox;
            field->setObjectName("dim_" + key);
            // Tamanhos aceitam 0 no campo para que a recusa seja explicada em accept().
            if (d.kind == DimensionKind::Angle)
                field->setRange(-kMaxAngle, kMaxAngle);
            else if (isSizeKey(d.key))
                field->setRange(0.0, kMaxSize);
            else
                field->setRange(-kMaxCoordinate, kMaxCoordinate);
            field->setDecimals(2);
            field->setSuffix(d.kind == DimensionKind::Angle ? tr(" °") : tr(" mm"));
            field->setKeyboardTracking(false);
            field->setMinimumWidth(140);
            field->setValue(d.value);
            connect(field, &QDoubleSpinBox::valueChanged, this, &SketchEntityDialog::updateComputed);
            form->addRow(label, field);
            m_fields.insert(key, field);
        }
        else
        {
            auto* value = new QLabel;
            value->setObjectName("dim_" + key);
            form->addRow(tr("%1 (calculado)").arg(QString::fromStdString(d.label)) + ':', value);
            m_computed.insert(key, value);
        }
    }

    m_message = new QLabel;
    m_message->setObjectName("mensagemValidacao");
    m_message->setWordWrap(true);
    m_message->setStyleSheet("color: #b00020;");
    m_message->hide();

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->setObjectName("botoesDimensoes");
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Aplicar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &SketchEntityDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &SketchEntityDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    QString note = tr("Coordenadas locais do plano do esboço. Sem restrições associativas.");
    if (std::holds_alternative<SketchArc>(entity.geometry))
        note = tr("Coordenadas locais do plano do esboço. Ângulos em graus a partir do eixo horizontal (H), positivos "
                  "no sentido anti-horário.");
    else if (std::holds_alternative<SketchChamfer>(entity.geometry))
        note = tr("Lado de referência: o primeiro lado do canto (ou o segundo, com a opção marcada). Duas distâncias: "
                  "D1 no lado de referência e D2 no outro. Distância e ângulo: D1 no lado de referência e ângulo entre "
                  "esse lado e o segmento do chanfro, medido por dentro do canto (D2 é calculada).");
    auto* noteLabel = new QLabel(note);
    noteLabel->setObjectName("rotuloConvencao");
    noteLabel->setWordWrap(true);
    layout->addWidget(noteLabel);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);
    updateComputed();
}

SketchGeometry SketchEntityDialog::geometry() const
{
    SketchGeometry g = m_entity.geometry;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
        applyDimension(g, it.key().toStdString(), it.value()->value());
    for (auto it = m_flags.cbegin(); it != m_flags.cend(); ++it)
        applyDimension(g, it.key().toStdString(), it.value()->isChecked() ? 1.0 : 0.0);
    for (auto it = m_choices.cbegin(); it != m_choices.cend(); ++it)
        applyDimension(g, it.key().toStdString(), double(it.value()->currentIndex()));
    return g;
}

QString SketchEntityDialog::validationMessage() const
{
    return m_message->isVisible() ? m_message->text() : QString();
}

void SketchEntityDialog::updateComputed()
{
    SketchEntity preview = m_entity;
    preview.geometry = geometry();
    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    for (const SketchDimension& d : dimensionsOf(preview))
        if (QLabel* label = m_computed.value(QString::fromStdString(d.key)))
            label->setText(locale.toString(d.value, 'f', 2) + (d.kind == DimensionKind::Angle ? tr("°") : tr(" mm")));
}

void SketchEntityDialog::accept()
{
    const EntityValidation v = SketchEntity::validate(geometry());
    if (!v.ok)
    {
        m_message->setText(QString::fromStdString(v.message));
        m_message->show();
        if (QDoubleSpinBox* field = m_fields.value(QString::fromStdString(v.field)))
        {
            field->setFocus();
            field->selectAll();
        }
        return; // permanece aberto
    }
    m_message->hide();
    QDialog::accept();
}

} // namespace cad::app
