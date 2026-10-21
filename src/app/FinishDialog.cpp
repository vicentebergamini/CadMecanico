#include "app/FinishDialog.h"

#include "modeling/ShapeInspector.h"

#include <QCheckBox>
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
    form->addRow(fillet ? tr("Raio:") : tr("Distância (simétrica):"), m_value);
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
