#include "app/ParametersDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace cad::app {

using cad::document::CadDocument;
using cad::document::PartParameters;

ParametersDialog::ParametersDialog(const PartParameters& current, QWidget* parent)
    : QDialog(parent)
{
    setObjectName("dialogoParametros");
    setWindowTitle(tr("Parâmetros da peça"));

    m_length = makeField("campoComprimento");
    m_width = makeField("campoLargura");
    m_height = makeField("campoAltura");
    m_holeRadius = makeField("campoRaioFuro");

    auto* form = new QFormLayout;
    form->addRow(tr("Comprimento (X):"), m_length);
    form->addRow(tr("Largura (Y):"), m_width);
    form->addRow(tr("Altura (Z):"), m_height);
    form->addRow(tr("Raio do furo:"), m_holeRadius);

    auto* note = new QLabel(tr("Furo passante vertical, centrado no bloco. Parede mínima: %1 mm.")
                                .arg(QLocale().toString(CadDocument::kMinWall, 'f', 1)));
    note->setWordWrap(true);

    m_message = new QLabel;
    m_message->setObjectName("mensagemValidacao");
    m_message->setWordWrap(true);
    m_message->setStyleSheet("color: #b00020;");
    m_message->hide();

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->setObjectName("botoesParametros");
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Aplicar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ParametersDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &ParametersDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);

    setParameters(current);
}

QDoubleSpinBox* ParametersDialog::makeField(const char* objectName)
{
    auto* field = new QDoubleSpinBox;
    field->setObjectName(objectName);
    // Faixa ampla de propósito: valores fora dos limites são recusados com mensagem em accept().
    field->setRange(0.0, CadDocument::kMaxDimension);
    field->setDecimals(2);
    field->setSingleStep(1.0);
    field->setSuffix(tr(" mm"));
    field->setKeyboardTracking(false);
    field->setMinimumWidth(140);
    return field;
}

PartParameters ParametersDialog::parameters() const
{
    PartParameters p;
    p.length = m_length->value();
    p.width = m_width->value();
    p.height = m_height->value();
    p.holeRadius = m_holeRadius->value();
    return p;
}

void ParametersDialog::setParameters(const PartParameters& p)
{
    m_length->setValue(p.length);
    m_width->setValue(p.width);
    m_height->setValue(p.height);
    m_holeRadius->setValue(p.holeRadius);
}

QString ParametersDialog::validationMessage() const
{
    return m_message->isVisible() ? m_message->text() : QString();
}

void ParametersDialog::accept()
{
    const auto result = CadDocument::validate(parameters());
    if (!result.ok)
    {
        m_message->setText(QString::fromStdString(result.message));
        m_message->show();
        QDoubleSpinBox* field = result.field == "length"  ? m_length
                                : result.field == "width" ? m_width
                                : result.field == "height" ? m_height
                                                           : m_holeRadius;
        field->setFocus();
        field->selectAll();
        return; // permanece aberto
    }
    m_message->hide();
    QDialog::accept();
}

} // namespace cad::app
