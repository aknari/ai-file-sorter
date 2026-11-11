#include "MainAppHelpActions.hpp"
#include "AppInfo.hpp"
#include "TranslationManager.hpp"

#include <app_version.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPixmap>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QString>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>

#include <array>

namespace {

struct QuickStartLanguageSuffix {
    Language language;
    const char* suffix;
};

constexpr std::array<QuickStartLanguageSuffix, 14> kQuickStartLanguageSuffixes{{
    {Language::Dutch, "_nl"},
    {Language::Swedish, "_sv"},
    {Language::Icelandic, "_is"},
    {Language::Norwegian, "_nb"},
    {Language::Finnish, "_fi"},
    {Language::Danish, "_da"},
    {Language::French, "_fr"},
    {Language::German, "_de"},
    {Language::Hindi, "_hi"},
    {Language::Italian, "_it"},
    {Language::Spanish, "_es"},
    {Language::Turkish, "_tr"},
    {Language::Korean, "_ko"},
    {Language::SimplifiedChinese, "_zh_cn"},
}};

QString support_page_url_string()
{
    return QStringLiteral("https://filesorter.app/donate/");
}

QString faq_page_url_string()
{
    return QStringLiteral("https://filesorter.app/faq/");
}

QString quick_start_fallback_markdown()
{
    return QStringLiteral(
        "# Quick Start Guide\n\n"
        "Use **Browse** to choose a folder, enable the options you need, run the "
        "analysis, and review the suggested moves and renames before applying them.");
}

QString quick_start_language_suffix(Language language)
{
    for (const auto& entry : kQuickStartLanguageSuffixes) {
        if (entry.language == language) {
            return QString::fromLatin1(entry.suffix);
        }
    }
    return {};
}

QString quick_start_resource_path(Language language)
{
    return QStringLiteral(":/dev/hfstudio/AIFileSorter/help/quick_start%1.md")
        .arg(quick_start_language_suffix(language));
}

QString platform_quick_start_resource_path(const QString& language_suffix)
{
#if defined(Q_OS_WIN)
    return QStringLiteral(":/dev/hfstudio/AIFileSorter/help/quick_start_windows%1.md")
        .arg(language_suffix);
#else
    (void)language_suffix;
    return {};
#endif
}

QString load_text_resource(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString filename_template_help_html(bool document_mode, const QString& current_template)
{
    const QString escaped_template = current_template.toHtmlEscaped();
    const bool spanish =
        TranslationManager::instance().current_language() == Language::Spanish;

    if (document_mode) {
        if (spanish) {
            return QStringLiteral(R"(
<h3>Plantilla de nombres para documentos</h3>
<p>El nombre sugerido se compone a partir de esta plantilla. Los tokens vacíos desaparecen junto con sus separadores y la extensión original se conserva.</p>
<table border="1" cellspacing="0" cellpadding="4">
<tr><th>Token</th><th>Significado</th></tr>
<tr><td><code>{date}</code></td><td>Fecha de creación del documento (PDF/DOCX; en algunos documentos solo se obtiene el año y el mes).</td></tr>
<tr><td><code>{year}</code></td><td>Los cuatro primeros dígitos de la fecha.</td></tr>
<tr><td><code>{type}</code></td><td>Tipo detectado por el modelo: factura, recibo, contrato, informe, carta, manual, notas, etc. Se limita a dos palabras.</td></tr>
<tr><td><code>{title}</code></td><td>Título tomado de los metadatos del documento (PDF/DOCX/ODF). Si no existe, se usa el asunto.</td></tr>
<tr><td><code>{subject}</code></td><td>Asunto descriptivo del contenido, limitado a tres palabras.</td></tr>
<tr><td><code>{original}</code></td><td>Nombre original del archivo, sin extensión.</td></tr>
</table>
<p><b>Plantilla actual:</b> <code>%1</code></p>
<p><b>Ejemplo:</b> <code>2024-05 factura Acme Enero.pdf</code></p>
<p>La plantilla se aplica cuando el análisis de documentos por contenido está activo y la opción de renombrar documentos está marcada. Si el nombre ya existe, se añaden sufijos <code>_1</code>, <code>_2</code>, etc.; nunca se sobrescribe un archivo.</p>
<p>Los ajustes se guardan en la sección <code>[Settings]</code> de <code>config.ini</code>: <code>DocumentFilenameTemplate</code>, <code>FilenameTemplateAsciiFold</code> y <code>FilenameTemplateMaxLength</code>.</p>
<p>Los archivos de audio y vídeo no usan esta plantilla: mantienen el formato <code>año_artista_álbum_título</code>.</p>
)").arg(escaped_template);
        }
        return QStringLiteral(R"(
<h3>Document filename template</h3>
<p>The suggested name is assembled from this template. Empty tokens disappear together with their separators, and the original extension is preserved.</p>
<table border="1" cellspacing="0" cellpadding="4">
<tr><th>Token</th><th>Meaning</th></tr>
<tr><td><code>{date}</code></td><td>Document creation date (PDF/DOCX; some documents only expose year and month).</td></tr>
<tr><td><code>{year}</code></td><td>The first four digits of the date.</td></tr>
<tr><td><code>{type}</code></td><td>Type detected by the model: invoice, receipt, contract, report, letter, manual, notes, etc. Limited to two words.</td></tr>
<tr><td><code>{title}</code></td><td>Title read from document metadata (PDF/DOCX/ODF). Falls back to the subject when absent.</td></tr>
<tr><td><code>{subject}</code></td><td>Short descriptive subject, limited to three words.</td></tr>
<tr><td><code>{original}</code></td><td>Original filename without its extension.</td></tr>
</table>
<p><b>Current template:</b> <code>%1</code></p>
<p><b>Example:</b> <code>2024-05 invoice Acme January.pdf</code></p>
<p>The template applies when document content analysis and the document rename option are enabled. If the name already exists, suffixes <code>_1</code>, <code>_2</code>, etc. are added; files are never overwritten.</p>
<p>Settings are stored in the <code>[Settings]</code> section of <code>config.ini</code>: <code>DocumentFilenameTemplate</code>, <code>FilenameTemplateAsciiFold</code> and <code>FilenameTemplateMaxLength</code>.</p>
<p>Audio and video files do not use this template: they keep the <code>year_artist_album_title</code> format.</p>
)").arg(escaped_template);
    }

    if (spanish) {
        return QStringLiteral(R"(
<h3>Plantilla de nombres para imágenes</h3>
<p>El nombre sugerido se compone a partir de esta plantilla. Los tokens vacíos desaparecen junto con sus separadores y la extensión original se conserva.</p>
<table border="1" cellspacing="0" cellpadding="4">
<tr><th>Token</th><th>Significado</th></tr>
<tr><td><code>{date}</code></td><td>Fecha de captura de los metadatos EXIF.</td></tr>
<tr><td><code>{time}</code></td><td>Hora de captura en formato compacto <code>HHMMSS</code>.</td></tr>
<tr><td><code>{year}</code></td><td>Los cuatro primeros dígitos de la fecha.</td></tr>
<tr><td><code>{place}</code></td><td>Lugar obtenido a partir de las coordenadas GPS. La primera vez requiere conexión a Internet; después se usa la caché local.</td></tr>
<tr><td><code>{subject}</code></td><td>Descripción visual generada por el modelo, limitada a tres palabras.</td></tr>
<tr><td><code>{original}</code></td><td>Nombre original del archivo, sin extensión.</td></tr>
</table>
<p><b>Plantilla actual:</b> <code>%1</code></p>
<p><b>Ejemplo:</b> <code>2014-03-10 Venecia patos negros.jpg</code></p>
<p>La plantilla se aplica cuando el análisis de imágenes por contenido está activo y la opción de renombrar imágenes está marcada. <code>{date}</code>, <code>{time}</code> y <code>{place}</code> solo se rellenan si está activa la opción «Añadir fecha y lugar de la foto al nombre».</p>
<p>Si el nombre ya existe, se añaden sufijos <code>_1</code>, <code>_2</code>, etc.; nunca se sobrescribe un archivo.</p>
<p>Los ajustes se guardan en la sección <code>[Settings]</code> de <code>config.ini</code>: <code>ImageFilenameTemplate</code>, <code>FilenameTemplateAsciiFold</code> y <code>FilenameTemplateMaxLength</code>.</p>
<p>Los archivos de audio y vídeo no usan esta plantilla: mantienen el formato <code>año_artista_álbum_título</code>.</p>
)").arg(escaped_template);
    }
    return QStringLiteral(R"(
<h3>Picture filename template</h3>
<p>The suggested name is assembled from this template. Empty tokens disappear together with their separators, and the original extension is preserved.</p>
<table border="1" cellspacing="0" cellpadding="4">
<tr><th>Token</th><th>Meaning</th></tr>
<tr><td><code>{date}</code></td><td>Capture date from EXIF metadata.</td></tr>
<tr><td><code>{time}</code></td><td>Capture time in compact <code>HHMMSS</code> form.</td></tr>
<tr><td><code>{year}</code></td><td>The first four digits of the date.</td></tr>
<tr><td><code>{place}</code></td><td>Place resolved from GPS coordinates. The first lookup needs network access; later lookups use the local cache.</td></tr>
<tr><td><code>{subject}</code></td><td>Visual description generated by the model, limited to three words.</td></tr>
<tr><td><code>{original}</code></td><td>Original filename without its extension.</td></tr>
</table>
<p><b>Current template:</b> <code>%1</code></p>
<p><b>Example:</b> <code>2014-03-10 Venice black ducks.jpg</code></p>
<p>The template applies when picture content analysis and the picture rename option are enabled. <code>{date}</code>, <code>{time}</code> and <code>{place}</code> are filled only when "Add photo date and place to filename" is enabled.</p>
<p>If the name already exists, suffixes <code>_1</code>, <code>_2</code>, etc. are added; files are never overwritten.</p>
<p>Settings are stored in the <code>[Settings]</code> section of <code>config.ini</code>: <code>ImageFilenameTemplate</code>, <code>FilenameTemplateAsciiFold</code> and <code>FilenameTemplateMaxLength</code>.</p>
<p>Audio and video files do not use this template: they keep the <code>year_artist_album_title</code> format.</p>
)").arg(escaped_template);
}

QString quick_start_markdown_for_language_impl(Language language)
{
    QString markdown;
    if (const QString localized = load_text_resource(quick_start_resource_path(language));
        !localized.isEmpty()) {
        markdown = localized;
    } else if (const QString english = load_text_resource(quick_start_resource_path(Language::English));
        !english.isEmpty()) {
        markdown = english;
    } else {
        markdown = quick_start_fallback_markdown();
    }

    const QString language_suffix = quick_start_language_suffix(language);
    QString platform = load_text_resource(platform_quick_start_resource_path(language_suffix));
    if (platform.isEmpty() && !language_suffix.isEmpty()) {
        platform = load_text_resource(platform_quick_start_resource_path(QString()));
    }
    if (!platform.isEmpty()) {
        markdown.append(QStringLiteral("\n\n"));
        markdown.append(platform);
    }

    return markdown;
}

bool open_external_url(const QUrl& url)
{
    if (QDesktopServices::openUrl(url)) {
        return true;
    }
#if defined(Q_OS_LINUX)
    return QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString(QUrl::FullyEncoded)});
#else
    return false;
#endif
}

} // namespace

void MainAppHelpActions::show_about(QWidget* parent)
{
    QDialog dialog(parent);
    const QString display_name = app_display_name();
    dialog.setWindowTitle(QObject::tr("About %1").arg(display_name));
    dialog.resize(600, 420);

    auto* layout = new QVBoxLayout(&dialog);
    auto* tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs);

    // About tab
    auto* about_tab = new QWidget(&dialog);
    auto* about_layout = new QVBoxLayout(about_tab);
    about_layout->setSpacing(8);

    if (QPixmap logo_pix(QStringLiteral(":/dev/hfstudio/AIFileSorter/images/logo.png")); !logo_pix.isNull()) {
        auto* logo_label = new QLabel(about_tab);
        logo_label->setAlignment(Qt::AlignHCenter);
        logo_label->setPixmap(logo_pix.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        about_layout->addWidget(logo_label);
    }

    auto* program_name = new QLabel(QStringLiteral("<h2>%1</h2>").arg(display_name.toHtmlEscaped()), about_tab);
    program_name->setAlignment(Qt::AlignHCenter);
    about_layout->addWidget(program_name);

    const QString version_text =
        QStringLiteral("Version: %1").arg(QString::fromStdString(APP_VERSION.to_string()));
    auto* version_label = new QLabel(version_text, about_tab);
    version_label->setAlignment(Qt::AlignHCenter);
    about_layout->addWidget(version_label);

    auto* copyright_label =
        new QLabel(QStringLiteral("Copyright 2026 HF Studio. All rights reserved."), about_tab);
    copyright_label->setAlignment(Qt::AlignHCenter);
    about_layout->addWidget(copyright_label);

    auto* website_label = new QLabel(QStringLiteral(
        "<a href=\"https://www.filesorter.app\">Visit the Website</a>"), about_tab);
    website_label->setOpenExternalLinks(true);
    website_label->setAlignment(Qt::AlignHCenter);
    about_layout->addWidget(website_label);

    about_layout->addStretch(1);
    tabs->addTab(about_tab, QObject::tr("About"));

    // Credits tab
    auto* credits_tab = new QWidget(&dialog);
    auto* credits_layout = new QVBoxLayout(credits_tab);
    credits_layout->setSpacing(8);

    if (QPixmap brand_logo_pix(QStringLiteral(":/dev/hfstudio/AIFileSorter/images/hf_logo.png"));
        !brand_logo_pix.isNull()) {
        auto* brand_logo = new QLabel(credits_tab);
        brand_logo->setAlignment(Qt::AlignHCenter);
        brand_logo->setPixmap(
            brand_logo_pix.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        credits_layout->addWidget(brand_logo);
    }

    auto* author_label = new QLabel(QStringLiteral("Author: hyperfield"), credits_tab);
    author_label->setAlignment(Qt::AlignHCenter);
    credits_layout->addWidget(author_label);

    auto* author_details = new QLabel(QStringLiteral(
        "Author's brand name is <a href=\"https://hfstudio.dev\">HF Studio</a>.<br>"
        "Source code on GitHub is <a href=\"https://github.com/hyperfield/ai-file-sorter\">here</a>."), credits_tab);
    author_details->setOpenExternalLinks(true);
    author_details->setAlignment(Qt::AlignHCenter);
    author_details->setWordWrap(true);
    credits_layout->addWidget(author_details);

    credits_layout->addStretch(1);
    tabs->addTab(credits_tab, QObject::tr("Credits"));

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(button_box);

    dialog.exec();
}

void MainAppHelpActions::show_filename_template_help(QWidget* parent,
                                                     bool document_mode,
                                                     const QString& current_template)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Filename template help"));
    dialog.resize(760, 620);

    auto* layout = new QVBoxLayout(&dialog);
    auto* browser = new QTextBrowser(&dialog);
    browser->setOpenExternalLinks(false);
    browser->setReadOnly(true);
    browser->setHtml(filename_template_help_html(document_mode, current_template));
    layout->addWidget(browser);

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(button_box);

    dialog.exec();
}

void MainAppHelpActions::show_quick_start(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Quick Start Guide"));
    dialog.resize(720, 560);

    auto* layout = new QVBoxLayout(&dialog);

    auto* browser = new QTextBrowser(&dialog);
    browser->setOpenExternalLinks(false);
    browser->setReadOnly(true);
    browser->setMarkdown(
        quick_start_markdown_for_language_impl(TranslationManager::instance().current_language()));
    layout->addWidget(browser);

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(button_box);

    dialog.exec();
}

void MainAppHelpActions::show_agpl_info(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("About the AGPL License"));
    dialog.resize(520, 320);

    auto* layout = new QVBoxLayout(&dialog);

    auto* summary = new QLabel(QObject::tr(
        "AI File Sorter is distributed under the GNU Affero General Public License v3.0."
        "<br><br>"
        "You can access the full source code at "
        "<a href=\"https://github.com/hyperfield/ai-file-sorter\">github.com/hyperfield/ai-file-sorter</a>."
        "<br><br>"
        "A full copy of the license is provided with this application and available online at "
        "<a href=\"https://www.gnu.org/licenses/agpl-3.0.html\">gnu.org</a>."), &dialog);
    summary->setTextFormat(Qt::RichText);
    summary->setOpenExternalLinks(true);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    layout->addStretch(1);

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(button_box);

    dialog.exec();
}

QString MainAppHelpActions::faq_page_url()
{
    return faq_page_url_string();
}

bool MainAppHelpActions::open_faq_page()
{
    return open_external_url(QUrl(faq_page_url_string()));
}

#ifdef AI_FILE_SORTER_TEST_BUILD
QString MainAppHelpActions::quick_start_markdown_for_language(Language language)
{
    return quick_start_markdown_for_language_impl(language);
}
#endif

QString MainAppHelpActions::support_page_url()
{
    return support_page_url_string();
}

bool MainAppHelpActions::open_support_page()
{
    const QUrl donation_url(support_page_url_string());
    return open_external_url(donation_url);
}
