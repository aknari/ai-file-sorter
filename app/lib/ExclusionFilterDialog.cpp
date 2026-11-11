#include "ExclusionFilterDialog.hpp"

#include "AnalysisExclusionFilter.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>

namespace {

QString join_values(const std::vector<std::string>& values)
{
    QStringList items;
    for (const auto& value : values) {
        items << QString::fromStdString(value);
    }
    return items.join(QStringLiteral(", "));
}

} // namespace

ExclusionFilterDialog::ExclusionFilterDialog(const std::vector<std::string>& extensions,
                                             const std::vector<std::string>& subfolders,
                                             const std::vector<std::string>& name_parts,
                                             QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Exclude files from analysis"));
    setModal(true);
    setSizeGripEnabled(true);
    resize(640, 300);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Files matching any rule below are skipped before analysis: they are not categorized, renamed, or moved."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    extensions_edit_ = new QLineEdit(join_values(extensions), this);
    extensions_edit_->setPlaceholderText(tr("tmp, log, *.bak"));
    form->addRow(tr("Extensions"), extensions_edit_);

    subfolders_edit_ = new QLineEdit(join_values(subfolders), this);
    subfolders_edit_->setPlaceholderText(tr("raw, .git, 2024/trips"));
    form->addRow(tr("Subfolders"), subfolders_edit_);

    name_parts_edit_ = new QLineEdit(join_values(name_parts), this);
    name_parts_edit_->setPlaceholderText(tr("backup, IMG_, draft"));
    form->addRow(tr("Name contains"), name_parts_edit_);

    layout->addLayout(form);

    auto* hint = new QLabel(
        tr("Separate values with commas. Matching ignores case. Subfolders are relative to the selected folder; use / to match a path."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* clear_button = buttons->addButton(tr("Clear all"), QDialogButtonBox::ResetRole);
    connect(clear_button, &QPushButton::clicked, this, [this]() {
        extensions_edit_->clear();
        subfolders_edit_->clear();
        name_parts_edit_->clear();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

std::vector<std::string> ExclusionFilterDialog::extensions() const
{
    return extensions_edit_ ? parse_exclusion_extensions(extensions_edit_->text().toStdString())
                            : std::vector<std::string>{};
}

std::vector<std::string> ExclusionFilterDialog::subfolders() const
{
    return subfolders_edit_ ? parse_exclusion_patterns(subfolders_edit_->text().toStdString())
                            : std::vector<std::string>{};
}

std::vector<std::string> ExclusionFilterDialog::name_parts() const
{
    return name_parts_edit_ ? parse_exclusion_patterns(name_parts_edit_->text().toStdString())
                            : std::vector<std::string>{};
}
