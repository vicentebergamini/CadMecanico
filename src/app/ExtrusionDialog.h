// Diálogo único da extrusão (E5.3/E5.6; unificado na E5.7 — P4): nome, esboço de origem, tipo de
// operação, corpo de destino, condição final, profundidade (mm), sentido e pré-visualização. O
// resultado de cada combinação de parâmetros é avaliado por uma função fornecida pela janela
// principal (CadDocument::previewExtrusion), que não altera o documento.
//
// Tipo de operação:
//   - Novo corpo: sólido independente (sem corpo de destino);
//   - Adicionar material (saliência): o volume é unido ao corpo de destino;
//   - Remover material (corte): o volume é subtraído do corpo de destino.
// "Unir resultado" (como "Merge result" do SOLIDWORKS) controla a união geométrica: marcado =
// adicionar material ao corpo de destino; desmarcado = novo corpo. No corte fica sempre marcado.
// "Cortar o lado de fora" (só no corte) remove o material fora do perfil dentro da extensão do
// corte, preservando o material dentro do perfil (ver ExtrudeParameters::cutOutside).
// Condições finais (E5.7.1 — P5): profundidade definida, plano médio, atravessar tudo (um ou dois
// sentidos), até a próxima face e até a face (selecionada no visualizador); Direção 2 opcional.
//
// Sentido: novo corpo e saliência seguem a normal do plano local do esboço (para fora da face);
// o corte segue o sentido oposto (para dentro da face). "Inverter sentido" troca o sentido.
#pragma once

#include "document/CadDocument.h"
#include "modeling/Features.h"

#include <QDialog>

#include <functional>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace cad::app {

class ExtrusionDialog : public QDialog
{
    Q_OBJECT

public:
    using Evaluator = std::function<cad::modeling::FeatureResult(const cad::document::ExtrudeParameters&)>;
    using BodyList = std::vector<std::pair<std::string, QString>>;
    // Nome padrão para cada tipo (o nome só é trocado se o usuário não o editou).
    using NameProvider = std::function<std::string(cad::document::ExtrudeMode)>;

    ExtrusionDialog(const cad::document::ExtrudeParameters& initial, const QString& sketchDescription,
                    Evaluator evaluator, bool editing, QWidget* parent = nullptr, const BodyList& bodies = {},
                    NameProvider defaultName = {});

    cad::document::ExtrudeParameters parameters() const;
    bool previewEnabled() const;
    // Mensagem atual (volume ou erro).
    QString resultMessage() const;
    // Tipo de operação atual (Novo corpo / Saliência / Corte).
    cad::document::ExtrudeMode mode() const;
    void setMode(cad::document::ExtrudeMode mode);

    // "Até a face" (E5.7.1): código de retorno de exec() quando o usuário pede para selecionar a
    // face limite no visualizador (o diálogo é reaberto em seguida com setLimitFace()).
    static constexpr int kPickLimitFace = 2;
    void setLimitFace(const std::string& bodyId, const cad::modeling::FaceSignature& face, const QString& description);

    static QString modeTitle(cad::document::ExtrudeMode mode, bool editing);

public slots:
    void accept() override;

signals:
    // Parâmetros alterados e resultado avaliado (para a pré-visualização no visualizador).
    void previewChanged(const cad::modeling::FeatureResult& result, bool show);

private:
    void evaluate();
    void updateWidgets();

    cad::document::ExtrudeParameters m_initial;
    Evaluator m_evaluator;
    NameProvider m_defaultName;
    bool m_editing = false;
    bool m_updating = false;
    std::string m_autoName; // último nome padrão aplicado (vazio se o usuário editou)
    QLineEdit* m_name = nullptr;
    QComboBox* m_mode = nullptr;
    QCheckBox* m_merge = nullptr;
    QCheckBox* m_outside = nullptr;
    QComboBox* m_target = nullptr;
    QComboBox* m_end = nullptr;
    QLabel* m_limitFace = nullptr;
    QPushButton* m_pickLimit = nullptr;
    QCheckBox* m_direction2 = nullptr;
    QComboBox* m_end2 = nullptr;
    QDoubleSpinBox* m_depth2 = nullptr;
    QDoubleSpinBox* m_depth = nullptr;
    QCheckBox* m_reversed = nullptr;
    QCheckBox* m_preview = nullptr;
    QLabel* m_direction = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_lastOk = false;
};

} // namespace cad::app
