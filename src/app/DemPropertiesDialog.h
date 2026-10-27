// Diálogo "Propriedades Físicas — Modelo MED" (E5.7.4a): propriedades do material e parâmetros
// do modelo MED do projeto, em unidades SI (ver dem/DemProperties.h). Só fecha com Aplicar se
// todos os valores forem números finitos e válidos (cad::dem::validate); Cancelar descarta a
// edição. O diálogo não altera o documento: quem o abre aplica o resultado.
#pragma once

#include "dem/DemProperties.h"

#include <QDialog>

#include <optional>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QScrollArea;

namespace cad::app {

class DemPropertiesDialog : public QDialog
{
    Q_OBJECT

public:
    // 'current' vazio = propriedades ainda não definidas no projeto (campos vazios).
    explicit DemPropertiesDialog(const std::optional<cad::dem::DemProperties>& current, QWidget* parent = nullptr);

    // Valores dos campos; vazio se algum campo não for um número finito.
    std::optional<cad::dem::DemProperties> properties() const;
    void setProperties(const std::optional<cad::dem::DemProperties>& properties);

    // Mensagem da última validação recusada (vazia se nenhuma).
    QString validationMessage() const;

    // Tamanho desejado (todo o conteúdo sem rolagem) antes de limitar à área útil do monitor com
    // DiscretizationDialog::fitGeometry (~90 % da altura, ~95 % da largura, centralizado).
    QSize preferredSize() const;

public slots:
    void accept() override;
    // Preenche os campos com os valores padrão de entrada de DEM_HIAGO_PRINCIPAL.m (não aplica).
    void fillMatlabDefaults();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QLineEdit* makeField(const char* objectName);
    QLineEdit* fieldFor(const std::string& key) const;
    void showMessage(const QString& text, QLineEdit* field);

    QLineEdit* m_density = nullptr;
    QLineEdit* m_poisson = nullptr;
    QLineEdit* m_young = nullptr;
    QLineEdit* m_fractureEnergy = nullptr;
    QLineEdit* m_failureFactor = nullptr;
    QLineEdit* m_corrcaf = nullptr;
    QScrollArea* m_area = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_placed = false;
};

} // namespace cad::app
