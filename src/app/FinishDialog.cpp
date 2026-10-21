#include "app/FinishDialog.h"

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
#include <QVBoxLayout>

namespace cad::app {

using cad::document::CadDocument;
using cad::document::OperationType;

FinishDialog::FinishDialog(OperationType type, const QString& name, double value, bool editing, QWidget* parent)
    : QDialog(parent)
    , m_type(type)
{
    const bool fillet = type == OperationType::Fillet3D;
    setObjectName("dialogoAcabamento3D");
    setWindowTitle(editing ? (fillet ? tr("Editar Fillet 3D") : tr("Editar Chamfer 3D"))
                           : (fillet ? tr("Fillet 3D") : tr("Chamfer 3D")));
    setModal(false);

    m_name = new QLineEdit(name);
    m_name->setObjectName("campoNomeAcabamento");
    m_name->setMaxLength(int(CadDocument::kMaxNameLength));

    m_body = new QLabel;
    m_body->setObjectName("rotuloCorpoOrigem");
    m_body->setWordWrap(true);

    m_edges = new QLabel;
    m_edges->setObjectName("rotuloArestas");

    m_value = new QDoubleSpinBox;
    m_value->setObjectName("campoValorAcabamento");
    // Faixa ampla: valores inválidos (zero, excessivos) são explicados pela avaliação.
    m_value->setRange(0.0, CadDocument::kMaxDimension);
    m_value->setDecimals(2);
    m_value->setSuffix(tr(" mm"));
    m_value->setKeyboardTracking(false);
    m_value->setValue(value);

    m_preview = new QCheckBox(tr("Pré-visualizar no visualizador 3D"));
    m_preview->setObjectName("opcaoPreVisualizarAcabamento");
    m_preview->setChecked(true);

    m_clear = new QPushButton(tr("Limpar seleção"));
    m_clear->setObjectName("botaoLimparSelecao");
    connect(m_clear, &QPushButton::clicked, this, &FinishDialog::clearSelectionRequested);

    m_message = new QLabel;
    m_message->setObjectName("mensagemAcabamento");
    m_message->setWordWrap(true);

    auto* hint = new QLabel(tr("Selecione as arestas no visualizador: clique seleciona, Shift + clique adiciona ou "
                               "remove. Faces e vértices não são selecionáveis neste modo."));
    hint->setWordWrap(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Nome:"), m_name);
    form->addRow(tr("Corpo de origem:"), m_body);
    form->addRow(tr("Arestas:"), m_edges);
    if (!fillet)
    {
        m_mode = new QComboBox;
        m_mode->setObjectName("campoModoChanfro");
        m_mode->addItem(tr("Distâncias iguais"), int(cad::document::Chamfer3DMode::Equal));
        m_mode->addItem(tr("Duas distâncias"), int(cad::document::Chamfer3DMode::TwoDistances));
        m_mode->addItem(tr("Distância e ângulo"), int(cad::document::Chamfer3DMode::DistanceAngle));
        form->addRow(tr("Modo:"), m_mode);
    }
    form->addRow(fillet ? tr("Raio:") : tr("Distância D1:"), m_value);
    if (!fillet)
    {
        m_value2 = new QDoubleSpinBox;
        m_value2->setObjectName("campoDistancia2");
        m_value2->setRange(0.0, CadDocument::kMaxDimension);
        m_value2->setDecimals(2);
        m_value2->setSuffix(tr(" mm"));
        m_value2->setKeyboardTracking(false);
        m_value2->setValue(value);
        m_angle = new QDoubleSpinBox;
        m_angle->setObjectName("campoAngulo");
        m_angle->setRange(0.0, 90.0);
        m_angle->setDecimals(2);
        m_angle->setSuffix(tr(" °"));
        m_angle->setKeyboardTracking(false);
        m_angle->setValue(45.0);
        m_flip = new QCheckBox(tr("Usar a outra face adjacente como referência"));
        m_flip->setObjectName("opcaoInverterReferencia");
        m_reference = new QLabel;
        m_reference->setObjectName("rotuloFaceReferencia");
        m_reference->setWordWrap(true);
        m_convention = new QLabel(tr("Convenção: D1 é medida na face de referência (destacada em azul); D2 na outra "
                                     "face. No modo distância e ângulo, o ângulo é medido entre a face de referência "
                                     "e a face do chanfro (0° < ângulo < 90°)."));
        m_convention->setObjectName("rotuloConvencaoChanfro");
        m_convention->setWordWrap(true);
        form->addRow(tr("Distância D2:"), m_value2);
        form->addRow(tr("Ângulo:"), m_angle);
        form->addRow(tr("Face de referência:"), m_reference);
        form->addRow(QString(), m_flip);
        form->addRow(m_convention);
        auto changed = [this] {
            updateChamferWidgets();
            emit chamferOptionsChanged();
            evaluate();
        };
        connect(m_mode, &QComboBox::currentIndexChanged, this, changed);
        connect(m_value2, &QDoubleSpinBox::valueChanged, this, changed);
        connect(m_angle, &QDoubleSpinBox::valueChanged, this, changed);
        connect(m_flip, &QCheckBox::toggled, this, changed);
        updateChamferWidgets();
    }
    form->addRow(QString(), m_preview);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->setObjectName("botoesAcabamento");
    m_buttons->button(QDialogButtonBox::Ok)->setText(editing ? tr("Aplicar") : tr("Criar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &FinishDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &FinishDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addLayout(form);
    layout->addWidget(m_clear, 0, Qt::AlignLeft);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);
    setMinimumWidth(420);

    connect(m_value, &QDoubleSpinBox::valueChanged, this, &FinishDialog::evaluate);
    connect(m_preview, &QCheckBox::toggled, this, &FinishDialog::evaluate);
    setSelectionInfo(0, QString(), QString());
}

void FinishDialog::setEvaluator(Evaluator evaluator)
{
    m_evaluator = std::move(evaluator);
    evaluate();
}

void FinishDialog::setSelectionInfo(int edgeCount, const QString& body, const QString& problem)
{
    m_edgeCount = edgeCount;
    m_problem = problem;
    m_edges->setText(edgeCount == 1 ? tr("1 aresta selecionada") : tr("%1 arestas selecionadas").arg(edgeCount));
    m_body->setText(body.isEmpty() ? tr("(selecione arestas de um corpo)") : body);
    m_clear->setEnabled(edgeCount > 0);
    evaluate();
}

double FinishDialog::value() const
{
    return m_value->value();
}

void FinishDialog::setChamferOptions(cad::document::Chamfer3DMode mode, double value2, double angle, bool flipReference)
{
    if (!m_mode)
        return;
    const QSignalBlocker b1(m_mode), b2(m_value2), b3(m_angle), b4(m_flip);
    m_mode->setCurrentIndex(m_mode->findData(int(mode)));
    m_value2->setValue(value2);
    m_angle->setValue(angle);
    m_flip->setChecked(flipReference);
    updateChamferWidgets();
    emit chamferOptionsChanged();
    evaluate();
}

cad::document::Chamfer3DMode FinishDialog::chamferMode() const
{
    return m_mode ? cad::document::Chamfer3DMode(m_mode->currentData().toInt()) : cad::document::Chamfer3DMode::Equal;
}

double FinishDialog::value2() const
{
    return m_value2 ? m_value2->value() : 0.0;
}

double FinishDialog::angle() const
{
    return m_angle ? m_angle->value() : 45.0;
}

bool FinishDialog::flipReference() const
{
    return m_flip && m_flip->isChecked();
}

void FinishDialog::setReferenceInfo(const QString& text)
{
    if (m_reference)
        m_reference->setText(text);
}

void FinishDialog::updateChamferWidgets()
{
    if (!m_mode)
        return;
    const auto mode = chamferMode();
    m_value2->setEnabled(mode == cad::document::Chamfer3DMode::TwoDistances);
    m_angle->setEnabled(mode == cad::document::Chamfer3DMode::DistanceAngle);
    m_flip->setEnabled(mode != cad::document::Chamfer3DMode::Equal);
    m_reference->setEnabled(mode != cad::document::Chamfer3DMode::Equal);
}

QString FinishDialog::name() const
{
    return m_name->text().trimmed();
}

QString FinishDialog::resultMessage() const
{
    return m_message->text();
}

void FinishDialog::evaluate()
{
    cad::modeling::FeatureResult r;
    if (m_edgeCount == 0)
        r.message = tr("Selecione pelo menos uma aresta no visualizador.").toStdString();
    else if (!m_problem.isEmpty())
        r.message = m_problem.toStdString();
    else if (m_evaluator)
        r = m_evaluator(m_value->value());
    m_lastOk = r.ok;
    if (r.ok)
    {
        const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
        m_message->setStyleSheet(QString());
        m_message->setText(tr("Resultado válido: volume %1 mm³")
                               .arg(locale.toString(cad::modeling::ShapeInspector::volume(r.shape), 'f', 2)));
    }
    else
    {
        m_message->setStyleSheet("color: #b00020;");
        m_message->setText(QString::fromStdString(r.message));
    }
    emit previewChanged(r, m_preview->isChecked());
}

void FinishDialog::accept()
{
    evaluate();
    if (!m_lastOk)
    {
        m_value->setFocus();
        return; // permanece aberto com a mensagem
    }
    QDialog::accept();
}

} // namespace cad::app
