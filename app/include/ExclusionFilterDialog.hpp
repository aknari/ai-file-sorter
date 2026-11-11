#ifndef EXCLUSION_FILTER_DIALOG_HPP
#define EXCLUSION_FILTER_DIALOG_HPP

#include <QCoreApplication>
#include <QDialog>

#include <string>
#include <vector>

class QLineEdit;

/**
 * @brief Edits the rules that skip files before analysis.
 */
class ExclusionFilterDialog final : public QDialog
{
    Q_DECLARE_TR_FUNCTIONS(ExclusionFilterDialog)
public:
    ExclusionFilterDialog(const std::vector<std::string>& extensions,
                          const std::vector<std::string>& subfolders,
                          const std::vector<std::string>& name_parts,
                          QWidget* parent = nullptr);

    std::vector<std::string> extensions() const;
    std::vector<std::string> subfolders() const;
    std::vector<std::string> name_parts() const;

private:
    QLineEdit* extensions_edit_{nullptr};
    QLineEdit* subfolders_edit_{nullptr};
    QLineEdit* name_parts_edit_{nullptr};
};

#endif // EXCLUSION_FILTER_DIALOG_HPP
