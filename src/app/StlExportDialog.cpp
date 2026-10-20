#include "app/StlExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace cad::app {

using namespace cad::io;

StlExportDialog::StlExportDialog(const QString& suggestedPath, const TopoDS_Shape& shape, QWidget* parent)
    : QDialog(parent)
    , m_shape(shape)
{
    setObjectName("dialogoExportarStl");
    setWindowTitle(tr("Exportar STL"));

    m_path = new QLineEdit(QDir::toNativeSeparators(suggestedPath));
    m_path->setObjectName("campoCaminhoStl");
    m_path->setMinimumWidth(360);
    auto* browseButton = new QPushButton(tr("Procurar..."));
    browseButton->setObjectName("botaoProcurarStl");
    connect(browseButton, &QPushButton::clicked, this, &StlExportDialog::browse);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path);
    pathRow->addWidget(browseButton);

    m_binary = new QRadioButton(tr("Binário (recomendado)"));
    m_binary->setObjectName("opcaoBinario");
    m_binary->setChecked(true);
    m_ascii = new QRadioButton(tr("ASCII"));
    m_ascii->setObjectName("opcaoAscii");
    auto* formatRow = new QHBoxLayout;
    formatRow->addWidget(m_binary);
    formatRow->addWidget(m_ascii);
    formatRow->addStretch();

    m_quality = new QComboBox;
    m_quality->setObjectName("campoQualidade");
    m_quality->addItem(tr("Baixa"), int(StlQuality::Low));
    m_quality->addItem(tr("Média"), int(StlQuality::Medium));
    m_quality->addItem(tr("Alta"), int(StlQuality::High));
    m_quality->setCurrentIndex(1);

    m_custom = new QCheckBox(tr("Tolerâncias personalizadas"));
    m_custom->setObjectName("opcaoTolerancias");
    m_linear = new QDoubleSpinBox;
    m_linear->setObjectName("campoTolLinear");
    m_linear->setRange(0.0, StlExporter::kMaxLinearDeflection); // validação explica valores fora dos limites
    m_linear->setDecimals(3);
    m_linear->setSingleStep(0.01);
    m_linear->setSuffix(tr(" mm"));
    m_angular = new QDoubleSpinBox;
    m_angular->setObjectName("campoTolAngular");
    m_angular->setRange(0.0, 180.0);
    m_angular->setDecimals(1);
    m_angular->setSuffix(tr(" °"));

    m_parameters = new QLabel;
    m_parameters->setObjectName("rotuloParametrosMalha");
    m_parameters->setWordWrap(true);

    auto* units = new QLabel(tr("Coordenadas exportadas em milímetros (unidade do documento), sem conversão. "
                                "O formato STL não armazena a unidade: informe-a ao abrir o arquivo em outro programa."));
    units->setObjectName("avisoUnidadeStl");
    units->setWordWrap(true);

    m_message = new QLabel;
    m_message->setObjectName("mensagemValidacao");
    m_message->setWordWrap(true);
    m_message->setStyleSheet("color: #b00020;");
    m_message->hide();

    auto* form = new QFormLayout;
    form->addRow(tr("Arquivo:"), pathRow);
    form->addRow(tr("Formato:"), formatRow);
    form->addRow(tr("Qualidade da malha:"), m_quality);
    form->addRow(QString(), m_custom);
    form->addRow(tr("Tolerância linear:"), m_linear);
    form->addRow(tr("Tolerância angular:"), m_angular);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->setObjectName("botoesExportarStl");
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Exportar"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &StlExportDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &StlExportDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_parameters);
    layout->addWidget(units);
    layout->addWidget(m_message);
    layout->addWidget(m_buttons);

    connect(m_quality, &QComboBox::currentIndexChanged, this, &StlExportDialog::updateParameters);
    connect(m_custom, &QCheckBox::toggled, this, &StlExportDialog::updateParameters);
    connect(m_linear, &QDoubleSpinBox::valueChanged, this, &StlExportDialog::updateParameters);
    connect(m_angular, &QDoubleSpinBox::valueChanged, this, &StlExportDialog::updateParameters);
    // Tolerâncias personalizadas começam com os valores do preset atual.
    const StlMeshParameters preset = StlExporter::presetParameters(StlQuality::Medium, m_shape);
    m_linear->setValue(preset.linearDeflection);
    m_angular->setValue(preset.angularDeflectionDeg);
    updateParameters();
}

QString StlExportDialog::filePath() const
{
    QString path = QDir::fromNativeSeparators(m_path->text().trimmed());
    if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".stl");
    return path;
}

StlExportOptions StlExportDialog::options() const
{
    StlExportOptions o;
    o.format = m_ascii->isChecked() ? StlFormat::Ascii : StlFormat::Binary;
    o.quality = StlQuality(m_quality->currentData().toInt());
    if (m_custom->isChecked())
    {
        o.linearDeflection = m_linear->value();
        o.angularDeflectionDeg = m_angular->value();
    }
    return o;
}

QString StlExportDialog::validationMessage() const
{
    return m_message->isVisible() ? m_message->text() : QString();
}

void StlExportDialog::browse()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar STL"), filePath(),
                                                      tr("Arquivos STL (*.stl)"), nullptr,
                                                      QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty())
        m_path->setText(QDir::toNativeSeparators(path));
}

void StlExportDialog::updateParameters()
{
    m_linear->setEnabled(m_custom->isChecked());
    m_angular->setEnabled(m_custom->isChecked());
    std::string error;
    const auto p = StlExporter::effectiveParameters(options(), m_shape, &error);
    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    if (p)
        m_parameters->setText(tr("Parâmetros efetivos: deflexão linear %1 mm, deflexão angular %2°.")
                                  .arg(locale.toString(p->linearDeflection, 'f', 4),
                                       locale.toString(p->angularDeflectionDeg, 'f', 1)));
    else
        m_parameters->setText(QString::fromStdString(error));
}

void StlExportDialog::accept()
{
    QString problem;
    if (filePath().isEmpty())
        problem = tr("Informe o arquivo de destino.");
    else
    {
        std::string error;
        if (!StlExporter::effectiveParameters(options(), m_shape, &error))
            problem = QString::fromStdString(error);
    }
    if (!problem.isEmpty())
    {
        m_message->setText(problem);
        m_message->show();
        return;
    }
    m_message->hide();
    QDialog::accept();
}

} // namespace cad::app
