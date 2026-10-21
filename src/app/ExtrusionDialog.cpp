#include "app/ExtrusionDialog.h"

#include "modeling/ShapeInspector.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace cad::app {

using cad::document::CadDocument;
using cad::document::EndCondition;
using cad::document::ExtrudeMode;
using cad::document::ExtrudeParameters;

QString ExtrusionDialog::modeTitle(ExtrudeMode mode, bool editing)
{
    switch (mode)
    {
    case ExtrudeMode::NewBody: return editing ? tr("Editar extrusão") : tr("Extrusão");
    case ExtrudeMode::Boss:    return editing ? tr("Editar extrusão de saliência") : tr("Extrusão de Saliência");
    case ExtrudeMode::Cut:     return editing ? tr("Editar extrusão de corte") : tr("Extrusão de Corte");
    }
    return QString();
}

ExtrusionDialog::ExtrusionDialog(const ExtrudeParameters& initial, const QString& sketchDescription, Evaluator evaluator,
                                 bool editing, QWidget* parent, const BodyList& bodies)
    : QDialog(parent)
    , m_initial(initial)
    , m_evaluator(std::move(evaluator))
{
    setObjectName("dialogoExtrusao");
    setWindowTitle(modeTitle(initial.mode, editing));

    m_name = new QLineEdit(QString::fromStdString(initial.name));
    m_name->setObjectName("campoNomeExtrusao");
    m_name->setMaxLength(int(CadDocument::kMaxNameLength));

    auto* sketch = new QLabel(sketchDescription);
    sketch->setObjectName("rotuloEsbocoOrigem");
    sketch->setWordWrap(true);

    m_depth = new QDoubleSpinBox;
    m_depth->setObjectName("campoProfundidade");
    // Faixa ampla de propósito: valores fora dos limites são explicados pela validação.
    m_depth->setRange(-CadDocument::kMaxExtrudeDepth, CadDocument::kMaxExtrudeDepth);
    m_depth->setDecimals(2);
    m_depth->setSuffix(tr(" mm"));
    m_depth->setKeyboardTracking(false);
    m_depth->setMinimumWidth(140);
    m_depth->setValue(initial.depth);

    m_reversed = new QCheckBox(initial.mode == ExtrudeMode::Cut ? tr("Inverter sentido (para fora da face)")
                                                                : tr("Inverter sentido (oposto à normal do plano)"));
    m_reversed->setObjectName("opcaoInverter");
    m_reversed->setChecked(initial.reversed);

    m_preview = new QCheckBox(tr("Pré-visualizar no visualizador 3D"));
    m_preview->setObjectName("opcaoPreVisualizar");
    m_preview->setChecked(true);

    m_direction = new QLabel(initial.mode == ExtrudeMode::Cut
                                 ? tr("Sentido do corte: para dentro da face (oposto à normal do plano do esboço). "
                                      "A região removida aparece em vermelho.")
                                 : tr("Sentido: normal do plano do esboço (para fora da face). A região adicionada "
                                      "aparece em verde."));
    m_direction->setObjectName("rotuloSentido");
    m_direction->setWordWrap(true);

    m_message = new QLabel;
    m_message->setObjectName("mensagemExtrusao");
    m_message->setWordWrap(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Nome:"), m_name);
    form->addRow(tr("Esboço de origem:"), sketch);
    if (initial.mode != ExtrudeMode::NewBody)
    {
        m_target = new QComboBox;
        m_target->setObjectName("campoCorpoDestino");
        for (const auto& [id, text] : bodies)
            m_target->addItem(text, QString::fromStdString(id));
        m_target->setCurrentIndex(std::max(0, m_target->findData(QString::fromStdString(initial.targetId))));
        form->addRow(tr("Corpo de destino:"), m_target);
        connect(m_target, &QComboBox::currentIndexChanged, this, &ExtrusionDialog::evaluate);
    }
    if (initial.mode == ExtrudeMode::Cut)
    {
        m_end = new QComboBox;
        m_end->setObjectName("campoCondicaoFinal");
        m_end->addItem(tr("Profundidade definida"), int(EndCondition::Blind));
        m_end->addItem(tr("Atravessar tudo"), int(EndCondition::ThroughAll));
        m_end->setCurrentIndex(m_end->findData(int(initial.endCondition)));
        form->addRow(tr("Condição final:"), m_end);
        connect(m_end, &QComboBox::currentIndexChanged, this, &ExtrusionDialog::evaluate);
    }
    form->addRow(tr("Profundidade:"), m_depth);
    form->addRow(QString(), m_reversed);
    form->addRow(QString(), m_preview);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->setObjectName("botoesExtrusao");
    m_buttons->button(QDialogButtonBox::Ok)->setText(editing ? tr("Aplicar") : tr("Extrudar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ExtrusionDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &ExtrusionDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_direction);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);
    setMinimumWidth(440);

    connect(m_depth, &QDoubleSpinBox::valueChanged, this, &ExtrusionDialog::evaluate);
    connect(m_reversed, &QCheckBox::toggled, this, &ExtrusionDialog::evaluate);
    connect(m_preview, &QCheckBox::toggled, this, &ExtrusionDialog::evaluate);
    evaluate();
}

ExtrudeParameters ExtrusionDialog::parameters() const
{
    ExtrudeParameters p = m_initial;
    p.name = m_name->text().trimmed().toStdString();
    p.depth = m_depth->value();
    p.reversed = m_reversed->isChecked();
    if (m_target)
        p.targetId = m_target->currentData().toString().toStdString();
    if (m_end)
        p.endCondition = EndCondition(m_end->currentData().toInt());
    return p;
}

bool ExtrusionDialog::previewEnabled() const
{
    return m_preview->isChecked();
}

QString ExtrusionDialog::resultMessage() const
{
    return m_message->text();
}

void ExtrusionDialog::evaluate()
{
    const ExtrudeParameters p = parameters();
    m_depth->setEnabled(p.endCondition != EndCondition::ThroughAll);
    const cad::modeling::FeatureResult r = m_evaluator ? m_evaluator(p) : cad::modeling::FeatureResult();
    m_lastOk = r.ok;
    if (r.ok)
    {
        const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
        m_message->setStyleSheet(QString());
        m_message->setText(tr("Sólido válido. Volume: %1 mm³")
                               .arg(locale.toString(cad::modeling::ShapeInspector::volume(r.shape), 'f', 2)));
    }
    else
    {
        m_message->setStyleSheet("color: #b00020;");
        m_message->setText(QString::fromStdString(r.message));
    }
    emit previewChanged(r, m_preview->isChecked());
}

void ExtrusionDialog::accept()
{
    evaluate();
    if (!m_lastOk)
    {
        m_depth->setFocus();
        m_depth->selectAll();
        return; // permanece aberto com a mensagem
    }
    QDialog::accept();
}

} // namespace cad::app