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
#include <QSignalBlocker>
#include <QStandardItemModel>
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
                                 bool editing, QWidget* parent, const BodyList& bodies, NameProvider defaultName)
    : QDialog(parent)
    , m_initial(initial)
    , m_evaluator(std::move(evaluator))
    , m_defaultName(std::move(defaultName))
    , m_editing(editing)
{
    setObjectName("dialogoExtrusao");

    m_name = new QLineEdit(QString::fromStdString(initial.name));
    m_name->setObjectName("campoNomeExtrusao");
    m_name->setMaxLength(int(CadDocument::kMaxNameLength));
    if (!editing && m_defaultName && m_defaultName(initial.mode) == initial.name)
        m_autoName = initial.name; // nome padrão: acompanha o tipo enquanto o usuário não o editar
    connect(m_name, &QLineEdit::textEdited, this, [this] { m_autoName.clear(); });

    auto* sketch = new QLabel(sketchDescription);
    sketch->setObjectName("rotuloEsbocoOrigem");
    sketch->setWordWrap(true);

    // Tipo de operação e opções de resultado.
    m_mode = new QComboBox;
    m_mode->setObjectName("campoTipoOperacao");
    m_mode->addItem(tr("Novo corpo"), int(ExtrudeMode::NewBody));
    m_mode->addItem(tr("Adicionar material (saliência)"), int(ExtrudeMode::Boss));
    m_mode->addItem(tr("Remover material (corte)"), int(ExtrudeMode::Cut));
    if (bodies.empty())
    {
        // Sem corpo no documento: só é possível criar um novo corpo.
        if (auto* model = qobject_cast<QStandardItemModel*>(m_mode->model()))
            for (int row : {1, 2})
                model->item(row)->setEnabled(false);
        m_mode->setToolTip(tr("Adicionar ou remover material exige um corpo sólido no documento"));
    }
    m_mode->setCurrentIndex(m_mode->findData(int(initial.mode)));

    m_merge = new QCheckBox(tr("Unir resultado ao corpo de destino"));
    m_merge->setObjectName("opcaoUnirResultado");
    m_merge->setToolTip(tr("Marcado: o volume é unido ao corpo de destino. Desmarcado: cria um corpo independente."));

    m_outside = new QCheckBox(tr("Cortar o lado de fora do perfil"));
    m_outside->setObjectName("opcaoCortarLadoDeFora");
    m_outside->setToolTip(tr("Remove o material fora do perfil dentro da extensão do corte e preserva o "
                             "material dentro do perfil."));
    m_outside->setChecked(initial.cutOutside);

    m_target = new QComboBox;
    m_target->setObjectName("campoCorpoDestino");
    for (const auto& [id, text] : bodies)
        m_target->addItem(text, QString::fromStdString(id));
    m_target->setCurrentIndex(std::max(0, m_target->findData(QString::fromStdString(initial.targetId))));

    m_end = new QComboBox;
    m_end->setObjectName("campoCondicaoFinal");
    for (EndCondition c : {EndCondition::Blind, EndCondition::MidPlane, EndCondition::ThroughAll,
                           EndCondition::ThroughAllBoth, EndCondition::UpToNext, EndCondition::UpToFace})
        m_end->addItem(tr(cad::document::endConditionLabel(c)), int(c));
    m_end->setCurrentIndex(m_end->findData(int(initial.endCondition)));

    // Até a face: face limite escolhida no visualizador (o diálogo é reaberto em seguida).
    m_limitFace = new QLabel(initial.limitBodyId.empty()
                                 ? tr("(nenhuma — selecione no visualizador)")
                                 : tr("%1 de %2").arg(QString::fromStdString(initial.limitFace.name),
                                                       QString::fromStdString(initial.limitBodyId)));
    m_limitFace->setObjectName("rotuloFaceLimite");
    m_limitFace->setWordWrap(true);
    m_pickLimit = new QPushButton(tr("Selecionar no visualizador..."));
    m_pickLimit->setObjectName("botaoSelecionarFaceLimite");
    connect(m_pickLimit, &QPushButton::clicked, this, [this] { done(kPickLimitFace); });

    // Direção 2: extensão adicional no sentido oposto, com condição própria.
    m_direction2 = new QCheckBox(tr("Direção 2 (sentido oposto)"));
    m_direction2->setObjectName("opcaoDirecao2");
    m_direction2->setChecked(initial.direction2);
    m_end2 = new QComboBox;
    m_end2->setObjectName("campoCondicaoFinal2");
    for (EndCondition c : {EndCondition::Blind, EndCondition::ThroughAll})
        m_end2->addItem(tr(cad::document::endConditionLabel(c)), int(c));
    m_end2->setCurrentIndex(m_end2->findData(int(initial.endCondition2)));
    m_depth2 = new QDoubleSpinBox;
    m_depth2->setObjectName("campoProfundidade2");
    m_depth2->setRange(-CadDocument::kMaxExtrudeDepth, CadDocument::kMaxExtrudeDepth);
    m_depth2->setDecimals(2);
    m_depth2->setSuffix(tr(" mm"));
    m_depth2->setKeyboardTracking(false);
    m_depth2->setValue(initial.depth2);

    m_depth = new QDoubleSpinBox;
    m_depth->setObjectName("campoProfundidade");
    // Faixa ampla de propósito: valores fora dos limites são explicados pela validação.
    m_depth->setRange(-CadDocument::kMaxExtrudeDepth, CadDocument::kMaxExtrudeDepth);
    m_depth->setDecimals(2);
    m_depth->setSuffix(tr(" mm"));
    m_depth->setKeyboardTracking(false);
    m_depth->setMinimumWidth(140);
    m_depth->setValue(initial.depth);

    m_reversed = new QCheckBox;
    m_reversed->setObjectName("opcaoInverter");
    m_reversed->setChecked(initial.reversed);

    m_preview = new QCheckBox(tr("Pré-visualizar no visualizador 3D"));
    m_preview->setObjectName("opcaoPreVisualizar");
    m_preview->setChecked(true);

    m_direction = new QLabel;
    m_direction->setObjectName("rotuloSentido");
    m_direction->setWordWrap(true);

    m_message = new QLabel;
    m_message->setObjectName("mensagemExtrusao");
    m_message->setWordWrap(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Nome:"), m_name);
    form->addRow(tr("Esboço de origem:"), sketch);
    form->addRow(tr("Tipo de operação:"), m_mode);
    form->addRow(QString(), m_merge);
    form->addRow(tr("Corpo de destino:"), m_target);
    form->addRow(tr("Condição final:"), m_end);
    form->addRow(tr("Face limite:"), m_limitFace);
    form->addRow(QString(), m_pickLimit);
    form->addRow(tr("Profundidade:"), m_depth);
    form->addRow(QString(), m_reversed);
    form->addRow(QString(), m_outside);
    form->addRow(QString(), m_direction2);
    form->addRow(tr("Condição da direção 2:"), m_end2);
    form->addRow(tr("Profundidade da direção 2:"), m_depth2);
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
    setMinimumWidth(460);

    // Tipo e "Unir resultado" ficam sincronizados: adicionar material sem união = novo corpo.
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        updateWidgets();
        evaluate();
    });
    connect(m_merge, &QCheckBox::toggled, this, [this](bool merge) {
        if (m_updating || mode() == ExtrudeMode::Cut)
            return;
        setMode(merge ? ExtrudeMode::Boss : ExtrudeMode::NewBody);
    });
    connect(m_outside, &QCheckBox::toggled, this, &ExtrusionDialog::evaluate);
    connect(m_target, &QComboBox::currentIndexChanged, this, &ExtrusionDialog::evaluate);
    connect(m_end, &QComboBox::currentIndexChanged, this, [this] {
        updateWidgets();
        evaluate();
    });
    connect(m_direction2, &QCheckBox::toggled, this, [this] {
        updateWidgets();
        evaluate();
    });
    connect(m_end2, &QComboBox::currentIndexChanged, this, [this] {
        updateWidgets();
        evaluate();
    });
    connect(m_depth2, &QDoubleSpinBox::valueChanged, this, &ExtrusionDialog::evaluate);
    connect(m_depth, &QDoubleSpinBox::valueChanged, this, &ExtrusionDialog::evaluate);
    connect(m_reversed, &QCheckBox::toggled, this, &ExtrusionDialog::evaluate);
    connect(m_preview, &QCheckBox::toggled, this, &ExtrusionDialog::evaluate);
    updateWidgets();
    evaluate();
}

ExtrudeMode ExtrusionDialog::mode() const
{
    return ExtrudeMode(m_mode->currentData().toInt());
}

void ExtrusionDialog::setMode(ExtrudeMode mode)
{
    m_mode->setCurrentIndex(m_mode->findData(int(mode)));
}

void ExtrusionDialog::setLimitFace(const std::string& bodyId, const cad::modeling::FaceSignature& face,
                                   const QString& description)
{
    m_initial.limitBodyId = bodyId;
    m_initial.limitFace = face;
    m_limitFace->setText(description);
    m_end->setCurrentIndex(m_end->findData(int(EndCondition::UpToFace)));
    evaluate();
}

void ExtrusionDialog::updateWidgets()
{
    m_updating = true;
    const ExtrudeMode m = mode();
    const bool cut = m == ExtrudeMode::Cut;
    setWindowTitle(modeTitle(m, m_editing));
    {
        const QSignalBlocker block(m_merge);
        m_merge->setChecked(m != ExtrudeMode::NewBody);
    }
    m_merge->setEnabled(!cut && m_target->count() > 0);
    m_target->setEnabled(m != ExtrudeMode::NewBody);
    // Condições que dependem do corpo de destino ficam indisponíveis no novo corpo.
    const bool hasTarget = m != ExtrudeMode::NewBody;
    if (auto* model = qobject_cast<QStandardItemModel*>(m_end->model()))
        for (EndCondition c : {EndCondition::ThroughAll, EndCondition::ThroughAllBoth, EndCondition::UpToNext})
            model->item(m_end->findData(int(c)))->setEnabled(hasTarget);
    const auto end = EndCondition(m_end->currentData().toInt());
    if (!hasTarget && (end == EndCondition::ThroughAll || end == EndCondition::ThroughAllBoth || end == EndCondition::UpToNext))
    {
        const QSignalBlocker block(m_end);
        m_end->setCurrentIndex(m_end->findData(int(EndCondition::Blind)));
    }
    const auto current = EndCondition(m_end->currentData().toInt());
    const bool prismatic = current == EndCondition::Blind || current == EndCondition::MidPlane
                           || current == EndCondition::ThroughAll || current == EndCondition::ThroughAllBoth;
    m_outside->setEnabled(cut && prismatic);
    if (!m_outside->isEnabled())
    {
        const QSignalBlocker block(m_outside);
        m_outside->setChecked(false);
    }
    m_limitFace->setEnabled(current == EndCondition::UpToFace);
    m_pickLimit->setEnabled(current == EndCondition::UpToFace);
    const bool dir2Allowed = current != EndCondition::MidPlane && current != EndCondition::ThroughAllBoth;
    m_direction2->setEnabled(dir2Allowed);
    if (!dir2Allowed)
    {
        const QSignalBlocker block(m_direction2);
        m_direction2->setChecked(false);
    }
    if (auto* model = qobject_cast<QStandardItemModel*>(m_end2->model()))
        model->item(m_end2->findData(int(EndCondition::ThroughAll)))->setEnabled(hasTarget);
    if (!hasTarget && m_end2->currentData().toInt() == int(EndCondition::ThroughAll))
    {
        const QSignalBlocker block(m_end2);
        m_end2->setCurrentIndex(m_end2->findData(int(EndCondition::Blind)));
    }
    m_end2->setEnabled(m_direction2->isChecked());
    m_depth2->setEnabled(m_direction2->isChecked() && m_end2->currentData().toInt() == int(EndCondition::Blind));
    m_reversed->setText(cut ? tr("Inverter sentido (para fora da face)") : tr("Inverter sentido (oposto à normal do plano)"));
    m_direction->setText(cut ? tr("Sentido do corte: para dentro da face (oposto à normal do plano do esboço). "
                                  "A região removida aparece em vermelho.")
                             : tr("Sentido: normal do plano do esboço (para fora da face). A região adicionada "
                                  "aparece em verde."));
    if (!m_autoName.empty() && m_defaultName)
    {
        m_autoName = m_defaultName(m);
        m_name->setText(QString::fromStdString(m_autoName));
    }
    m_updating = false;
}

ExtrudeParameters ExtrusionDialog::parameters() const
{
    ExtrudeParameters p = m_initial;
    p.name = m_name->text().trimmed().toStdString();
    p.mode = mode();
    p.depth = m_depth->value();
    p.reversed = m_reversed->isChecked();
    p.targetId = p.mode == ExtrudeMode::NewBody ? std::string() : m_target->currentData().toString().toStdString();
    p.endCondition = EndCondition(m_end->currentData().toInt());
    p.cutOutside = p.mode == ExtrudeMode::Cut && m_outside->isChecked();
    p.direction2 = m_direction2->isChecked();
    p.endCondition2 = EndCondition(m_end2->currentData().toInt());
    p.depth2 = m_depth2->value();
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
    if (m_updating)
        return;
    const ExtrudeParameters p = parameters();
    m_depth->setEnabled(p.endCondition == EndCondition::Blind || p.endCondition == EndCondition::MidPlane);
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
