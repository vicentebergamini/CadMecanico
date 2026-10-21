#include "app/BooleanDialog.h"

#include "modeling/ShapeInspector.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace cad::app {

using cad::document::BooleanParameters;
using cad::document::CadDocument;
using cad::modeling::BooleanKind;

namespace {

QString colorTag(const char* color)
{
    return QStringLiteral("<span style=\"color:%1;\">&#9632;</span> ").arg(QLatin1String(color));
}

} // namespace

QString BooleanDialog::operandLabel(BooleanKind kind, bool first)
{
    if (kind == BooleanKind::Cut)
        return first ? tr("Corpo principal (A)") : tr("Ferramenta de corte (B)");
    return first ? tr("Corpo A") : tr("Corpo B");
}

BooleanDialog::BooleanDialog(const BooleanParameters& initial, const BodyList& bodies, Evaluator evaluator,
                             bool editing, QWidget* parent)
    : QDialog(parent)
    , m_initial(initial)
    , m_evaluator(std::move(evaluator))
{
    setObjectName("dialogoBooleana");
    setWindowTitle(editing ? tr("Editar operação booleana") : tr("Operação booleana"));

    m_name = new QLineEdit(QString::fromStdString(initial.name));
    m_name->setObjectName("campoNomeBooleana");
    m_name->setMaxLength(int(CadDocument::kMaxNameLength));

    m_kind = new QComboBox;
    m_kind->setObjectName("campoTipoBooleana");
    m_kind->addItem(tr("União (A ∪ B)"), int(BooleanKind::Fuse));
    m_kind->addItem(tr("Subtração (A − B)"), int(BooleanKind::Cut));
    m_kind->addItem(tr("Interseção (A ∩ B)"), int(BooleanKind::Common));
    m_kind->setCurrentIndex(m_kind->findData(int(initial.kind)));

    m_a = new QComboBox;
    m_a->setObjectName("campoCorpoA");
    m_b = new QComboBox;
    m_b->setObjectName("campoCorpoB");
    for (const auto& [id, text] : bodies)
    {
        m_a->addItem(text, QString::fromStdString(id));
        m_b->addItem(text, QString::fromStdString(id));
    }
    m_a->setCurrentIndex(m_a->findData(QString::fromStdString(initial.operandA)));
    m_b->setCurrentIndex(m_b->findData(QString::fromStdString(initial.operandB)));

    m_labelA = new QLabel;
    m_labelA->setObjectName("rotuloCorpoA");
    m_labelB = new QLabel;
    m_labelB->setObjectName("rotuloCorpoB");

    m_message = new QLabel;
    m_message->setObjectName("mensagemBooleana");
    m_message->setWordWrap(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Nome:"), m_name);
    form->addRow(tr("Operação:"), m_kind);
    form->addRow(m_labelA, m_a);
    form->addRow(m_labelB, m_b);

    auto* legend = new QLabel(tr("No visualizador: %1A em azul, %2B em vermelho. Os corpos usados ficam no histórico "
                                 "e deixam de ser exibidos e exportados após a operação.")
                                  .arg(colorTag("#2666f2"), colorTag("#f2332a")));
    legend->setObjectName("legendaOperandos");
    legend->setTextFormat(Qt::RichText);
    legend->setWordWrap(true);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->setObjectName("botoesBooleana");
    m_buttons->button(QDialogButtonBox::Ok)->setText(editing ? tr("Aplicar") : tr("Criar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &BooleanDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &BooleanDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(legend);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);
    setMinimumWidth(460);

    connect(m_kind, &QComboBox::currentIndexChanged, this, &BooleanDialog::evaluate);
    connect(m_a, &QComboBox::currentIndexChanged, this, &BooleanDialog::evaluate);
    connect(m_b, &QComboBox::currentIndexChanged, this, &BooleanDialog::evaluate);
    evaluate();
}

BooleanParameters BooleanDialog::parameters() const
{
    BooleanParameters p = m_initial;
    p.name = m_name->text().trimmed().toStdString();
    p.kind = BooleanKind(m_kind->currentData().toInt());
    p.operandA = m_a->currentData().toString().toStdString();
    p.operandB = m_b->currentData().toString().toStdString();
    return p;
}

QString BooleanDialog::resultMessage() const
{
    return m_message->text();
}

void BooleanDialog::evaluate()
{
    const BooleanParameters p = parameters();
    m_labelA->setText(colorTag("#2666f2") + operandLabel(p.kind, true) + ':');
    m_labelB->setText(colorTag("#f2332a") + operandLabel(p.kind, false) + ':');
    emit operandsChanged(QString::fromStdString(p.operandA), QString::fromStdString(p.operandB));

    const cad::modeling::FeatureResult r = m_evaluator ? m_evaluator(p) : cad::modeling::FeatureResult();
    m_lastOk = r.ok;
    if (r.ok)
    {
        const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
        const int solids = cad::modeling::countSolids(r.shape);
        const QString volume = locale.toString(cad::modeling::ShapeInspector::volume(r.shape), 'f', 2);
        m_message->setStyleSheet(QString());
        m_message->setText(solids == 1 ? tr("Resultado válido: 1 sólido, volume %1 mm³").arg(volume)
                                       : tr("Resultado válido: %1 sólidos desconectados (corpo composto), volume total "
                                            "%2 mm³")
                                             .arg(solids)
                                             .arg(volume));
    }
    else
    {
        m_message->setStyleSheet("color: #b00020;");
        m_message->setText(QString::fromStdString(r.message));
    }
}

void BooleanDialog::accept()
{
    evaluate();
    if (!m_lastOk)
        return; // permanece aberto com a mensagem
    QDialog::accept();
}

} // namespace cad::app
