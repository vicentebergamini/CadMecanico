#include "app/DemPropertiesDialog.h"

#include "app/DiscretizationDialog.h"
#include "dem/DemUnits.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace cad::app {

using cad::dem::DemProperties;
using cad::dem::ModelParameters;

namespace {

// Número com a menor representação que preserva o valor (ponto decimal, sem separador de milhar).
QString formatNumber(double value)
{
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

QLabel* unitLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(objectName);
    return label;
}

// Campo somente leitura (CORRCAF e grandezas derivadas).
void makeReadOnly(QLineEdit* field, const QString& toolTip)
{
    field->setReadOnly(true);
    field->setEnabled(false);
    field->setPlaceholderText(QString());
    field->setToolTip(toolTip);
}

QLabel* noteLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

} // namespace

DemPropertiesDialog::DemPropertiesDialog(const std::optional<DemProperties>& current, QWidget* parent)
    : QDialog(parent)
{
    setObjectName("dialogoPropriedadesMED");
    setWindowTitle(tr("Propriedades Físicas — Modelo MED"));
    setSizeGripEnabled(true);

    m_density = makeField("campoDensidade");
    m_poisson = makeField("campoPoisson");
    m_young = makeField("campoModuloElasticidade");
    m_fractureEnergy = makeField("campoEnergiaFratura");
    m_equivalentDefect = makeField("campoDeq");
    m_corrcaf = makeField("campoCORRCAF");
    m_corrcaf->setText(formatNumber(ModelParameters::corrcaf()));
    makeReadOnly(m_corrcaf, tr("Valor obrigatório do modelo; não editável"));
    m_rfc = makeField("campoRFC");
    makeReadOnly(m_rfc, tr("Calculado: RFC = 1/√d_eq"));
    m_poa = makeField("campoPOA");
    makeReadOnly(m_poa, tr("Calculado: POA = 1 − ν²"));
    m_epr = makeField("campoEPR");
    makeReadOnly(m_epr, tr("Calculado: EPR = √(GRF/(d_eq·E·POA))"));

    // Material: rótulo | campo | unidade.
    auto* materialBox = new QGroupBox(tr("Propriedades do material"));
    materialBox->setObjectName("grupoMaterial");
    auto* material = new QGridLayout(materialBox);
    int row = 0;
    material->addWidget(new QLabel(tr("Densidade (ρ):")), row, 0);
    material->addWidget(m_density, row, 1);
    material->addWidget(unitLabel(tr("kg/m³"), "unidadeDensidade"), row++, 2);
    material->addWidget(new QLabel(tr("Coeficiente de Poisson (ν):")), row, 0);
    material->addWidget(m_poisson, row, 1);
    material->addWidget(unitLabel(tr("adimensional"), "unidadePoisson"), row++, 2);
    material->addWidget(new QLabel(tr("Módulo de elasticidade (E):")), row, 0);
    material->addWidget(m_young, row, 1);
    material->addWidget(unitLabel(tr("Pa (N/m²)"), "unidadeModuloElasticidade"), row++, 2);
    material->addWidget(new QLabel(tr("Energia específica de fratura por área (GRF):")), row, 0);
    material->addWidget(m_fractureEnergy, row, 1);
    material->addWidget(unitLabel(tr("N/m (J/m²)"), "unidadeEnergiaFratura"), row++, 2);
    material->addWidget(noteLabel(tr("GRF é a única entrada de energia de fratura do modelo MATLAB. A energia de "
                                     "fratura em N·m não é entrada do modelo e não é calculada nesta etapa."),
                                  "notaEnergiaFratura"),
                        row++, 0, 1, 3);
    material->setColumnStretch(1, 1);

    // Parâmetros do modelo MED.
    auto* modelBox = new QGroupBox(tr("Parâmetros do modelo MED"));
    modelBox->setObjectName("grupoModeloMED");
    auto* model = new QGridLayout(modelBox);
    row = 0;
    model->addWidget(new QLabel(tr("Defeito intrínseco equivalente (d_eq):")), row, 0);
    model->addWidget(m_equivalentDefect, row, 1);
    model->addWidget(unitLabel(tr("m"), "unidadeDeq"), row++, 2);
    model->addWidget(new QLabel(tr("Fator de compatibilização de malha (CORRCAF):")), row, 0);
    model->addWidget(m_corrcaf, row, 1);
    model->addWidget(unitLabel(tr("adimensional (fixo)"), "unidadeCORRCAF"), row++, 2);
    model->setColumnStretch(1, 1);

    // Grandezas derivadas (somente leitura, recalculadas a cada edição; não são gravadas).
    auto* derivedBox = new QGroupBox(tr("Grandezas calculadas"));
    derivedBox->setObjectName("grupoGrandezasCalculadas");
    auto* derived = new QGridLayout(derivedBox);
    row = 0;
    derived->addWidget(new QLabel(tr("Fator de falha (RFC = 1/√d_eq):")), row, 0);
    derived->addWidget(m_rfc, row, 1);
    derived->addWidget(unitLabel(tr("m^(-1/2)"), "unidadeRFC"), row++, 2);
    derived->addWidget(new QLabel(tr("POA = 1 − ν²:")), row, 0);
    derived->addWidget(m_poa, row, 1);
    derived->addWidget(unitLabel(tr("adimensional"), "unidadePOA"), row++, 2);
    derived->addWidget(new QLabel(tr("Deformação de pico de referência (EPR):")), row, 0);
    derived->addWidget(m_epr, row, 1);
    derived->addWidget(unitLabel(tr("adimensional"), "unidadeEPR"), row++, 2);
    derived->addWidget(noteLabel(tr("EPR = √(GRF/(d_eq·E·POA)), equivalente a RFC·√(GRF/(E·POA)) do modelo MATLAB. "
                                    "Os campos mostram \"—\" enquanto as entradas não forem válidas."),
                                 "notaGrandezasCalculadas"),
                       row++, 0, 1, 3);
    derived->setColumnStretch(1, 1);

    auto* content = new QWidget;
    content->setObjectName("conteudoPropriedadesMED");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->addWidget(materialBox);
    contentLayout->addWidget(modelBox);
    contentLayout->addWidget(derivedBox);
    contentLayout->addWidget(noteLabel(tr("Modelo físico em unidades SI (m, kg, s, N). A geometria CAD permanece em "
                                          "milímetros; a conversão para metros (1 mm = 0,001 m) é feita apenas no "
                                          "modelo físico."),
                                       "notaUnidades"));
    contentLayout->addStretch(1);

    // Conteúdo rolável; mensagem e botões sempre visíveis abaixo dele.
    m_area = new QScrollArea;
    m_area->setObjectName("areaPropriedadesMED");
    m_area->setWidgetResizable(true);
    m_area->setFrameShape(QFrame::NoFrame);
    m_area->setWidget(content);
    // Nunca mais estreita que o conteúdo: no tamanho mínimo só há rolagem vertical.
    m_area->setMinimumWidth(content->minimumSizeHint().width() + style()->pixelMetric(QStyle::PM_ScrollBarExtent)
                            + 2 * m_area->frameWidth());

    m_message = new QLabel;
    m_message->setObjectName("mensagemValidacao");
    m_message->setWordWrap(true);
    m_message->setStyleSheet("color: #b00020;");
    m_message->hide();

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    m_buttons->setObjectName("botoesPropriedadesMED");
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Aplicar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    QPushButton* defaults = m_buttons->button(QDialogButtonBox::RestoreDefaults);
    defaults->setText(tr("Valores padrão do MATLAB"));
    defaults->setObjectName("botaoPadraoMATLAB");
    defaults->setToolTip(tr("Preenche os campos com os valores padrão de entrada de DEM_HIAGO_PRINCIPAL.m "
                            "(ρ = 1240, ν = 0,36, E = 3,5e9, GRF = 2400) e d_eq = 0,01 m (RFC = 10); nada é aplicado até "
                            "clicar em Aplicar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &DemPropertiesDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &DemPropertiesDialog::reject);
    connect(defaults, &QPushButton::clicked, this, &DemPropertiesDialog::fillMatlabDefaults);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_area, 1);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);

    // ρ não entra nas fórmulas, mas os campos calculados só são exibidos com todas as entradas válidas.
    for (QLineEdit* input : {m_density, m_poisson, m_young, m_fractureEnergy, m_equivalentDefect})
        connect(input, &QLineEdit::textChanged, this, &DemPropertiesDialog::updateDerived);

    setProperties(current);
}

QLineEdit* DemPropertiesDialog::makeField(const char* objectName)
{
    auto* field = new QLineEdit;
    field->setObjectName(objectName);
    field->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("0.000000000e+00")) + 16);
    field->setPlaceholderText(tr("não definido"));
    return field;
}

void DemPropertiesDialog::setProperties(const std::optional<DemProperties>& p)
{
    m_density->setText(p ? formatNumber(p->material.density) : QString());
    m_poisson->setText(p ? formatNumber(p->material.poissonRatio) : QString());
    m_young->setText(p ? formatNumber(p->material.youngModulus) : QString());
    m_fractureEnergy->setText(p ? formatNumber(p->material.fractureEnergy) : QString());
    m_equivalentDefect->setText(p ? formatNumber(p->model.equivalentDefect) : QString());
    updateDerived();
}

void DemPropertiesDialog::updateDerived()
{
    // Só com todas as entradas numéricas e válidas; caso contrário "—".
    const auto p = properties();
    const bool valid = p && cad::dem::validate(*p).ok;
    const cad::dem::DerivedParameters d = valid ? cad::dem::derive(*p) : cad::dem::DerivedParameters{};
    const QString none = QStringLiteral("—");
    m_rfc->setText(valid ? formatNumber(d.rfc) : none);
    m_poa->setText(valid ? formatNumber(d.poa) : none);
    m_epr->setText(valid ? formatNumber(d.epr) : none);
}

void DemPropertiesDialog::fillMatlabDefaults()
{
    setProperties(cad::dem::matlabDefaults());
    m_message->hide();
}

std::optional<DemProperties> DemPropertiesDialog::properties() const
{
    DemProperties p;
    const std::pair<QLineEdit*, double*> fields[] = {
        {m_density, &p.material.density},
        {m_poisson, &p.material.poissonRatio},
        {m_young, &p.material.youngModulus},
        {m_fractureEnergy, &p.material.fractureEnergy},
        {m_equivalentDefect, &p.model.equivalentDefect},
    };
    for (const auto& [field, value] : fields)
        if (!cad::dem::parseNumber(field->text().toStdString(), *value))
            return std::nullopt;
    return p;
}

QLineEdit* DemPropertiesDialog::fieldFor(const std::string& key) const
{
    if (key == cad::dem::kFieldDensity)
        return m_density;
    if (key == cad::dem::kFieldPoissonRatio)
        return m_poisson;
    if (key == cad::dem::kFieldYoungModulus)
        return m_young;
    if (key == cad::dem::kFieldFractureEnergy)
        return m_fractureEnergy;
    return m_equivalentDefect;
}

QString DemPropertiesDialog::validationMessage() const
{
    return m_message->isVisible() ? m_message->text() : QString();
}

void DemPropertiesDialog::showMessage(const QString& text, QLineEdit* field)
{
    m_message->setText(text);
    m_message->show();
    field->setFocus();
    field->selectAll();
    m_area->ensureWidgetVisible(field);
}

void DemPropertiesDialog::accept()
{
    // Cada campo precisa ser um número finito (texto vazio, não numérico, NaN e infinito recusados).
    const std::pair<QLineEdit*, QString> fields[] = {
        {m_density, tr("Densidade ρ")},
        {m_poisson, tr("Coeficiente de Poisson ν")},
        {m_young, tr("Módulo de elasticidade E")},
        {m_fractureEnergy, tr("Energia específica de fratura GRF")},
        {m_equivalentDefect, tr("Defeito intrínseco equivalente d_eq")},
    };
    for (const auto& [field, label] : fields)
    {
        double value = 0.0;
        if (!cad::dem::parseNumber(field->text().toStdString(), value))
        {
            showMessage(field->text().trimmed().isEmpty()
                            ? tr("%1: informe um valor.").arg(label)
                            : tr("%1: \"%2\" não é um número válido (use, por exemplo, 3.5e9 ou 0,36).")
                                  .arg(label, field->text().trimmed()),
                        field);
            return; // permanece aberto
        }
    }
    const auto result = cad::dem::validate(*properties());
    if (!result.ok)
    {
        showMessage(QString::fromStdString(result.message), fieldFor(result.field));
        return;
    }
    m_message->hide();
    QDialog::accept();
}

QSize DemPropertiesDialog::preferredSize() const
{
    // Conteúdo inteiro sem rolagem (a área de rolagem só entra em ação em telas pequenas) mais a
    // mensagem e os botões; largura com folga para a barra de rolagem vertical.
    const QSize content = m_area->widget()->sizeHint();
    const QSize hint = sizeHint();
    const int extraHeight = std::max(0, content.height() - m_area->sizeHint().height());
    const int scrollBar = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    return {std::max(hint.width(), content.width() + scrollBar + 2 * layout()->contentsMargins().left()),
            hint.height() + extraHeight};
}

void DemPropertiesDialog::showEvent(QShowEvent* event)
{
    // Tamanho inicial: o do conteúdo, limitado à área útil do monitor da janela principal (o
    // excedente rola) e centralizado — mesma regra da janela de discretização.
    if (!m_placed && !event->spontaneous())
    {
        m_placed = true;
        QScreen* scr = parentWidget() ? parentWidget()->window()->screen() : screen();
        if (!scr)
            scr = QGuiApplication::primaryScreen();
        if (scr)
        {
            QMargins frame;
            if (QWindow* handle = windowHandle())
                frame = handle->frameMargins();
            if (frame.isNull())
                frame = QMargins(8, fontMetrics().height() * 2 + 8, 8, 8);
            setGeometry(DiscretizationDialog::fitGeometry(preferredSize(), minimumSizeHint().expandedTo(minimumSize()),
                                                          scr->availableGeometry(), frame));
        }
    }
    QDialog::showEvent(event);
}

} // namespace cad::app
