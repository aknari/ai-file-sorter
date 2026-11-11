#include "DocumentTextAnalyzer.hpp"

#include "ILLMClient.hpp"
#include "FilenameTemplate.hpp"
#include "Utils.hpp"

#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>

#if defined(AI_FILE_SORTER_USE_PDFIUM)
#include "fpdf_doc.h"
#include "fpdf_text.h"
#include "fpdfview.h"
#endif
#if defined(AI_FILE_SORTER_USE_LIBZIP)
#include <zip.h>
#endif
#if defined(AI_FILE_SORTER_USE_PUGIXML)
#include <pugixml.hpp>
#endif

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers not found. Install jsoncpp development files."
#endif

#include <array>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr size_t kMaxProcessOutput = 200000;
constexpr std::size_t kZipMemberReadBufferBytes = 4096;
constexpr int kPdfiumTextChunkChars = 4096;
constexpr int kPdfNameMaxPages = 6;
constexpr int kPdfNameMaxEvidenceChars = 16000;
constexpr int kPdfNameMinPageChars = 600;
constexpr int kPdfNameMaxTokens = 400;

struct PdfPageText {
    int page = 0;
    std::string text;
};

QString path_to_qstring(const std::filesystem::path& path) {
    const std::string utf8 = Utils::path_to_utf8(path);
    return QString::fromUtf8(utf8.c_str(), static_cast<qsizetype>(utf8.size()));
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_copy(const std::string& value) {
    auto result = value;
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), not_space));
    result.erase(std::find_if(result.rbegin(), result.rend(), not_space).base(), result.end());
    return result;
}

std::string collapse_whitespace(const std::string& value) {
    std::string collapsed;
    collapsed.reserve(value.size());
    bool last_space = false;
    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            if (!last_space) {
                collapsed.push_back(' ');
                last_space = true;
            }
        } else {
            collapsed.push_back(static_cast<char>(ch));
            last_space = false;
        }
    }
    return trim_copy(collapsed);
}

QString sanitize_utf8_text(const std::string& value) {
    QString cleaned = QString::fromUtf8(value.c_str());
    cleaned.remove(QChar::ReplacementCharacter);
    return cleaned.normalized(QString::NormalizationForm_C);
}

std::string clean_pdf_page_text(const QString& input) {
    QString text = input;
    text.replace(QChar('\r'), QChar('\n'));
    QStringList lines;
    for (const QString& raw_line : text.split('\n')) {
        const QString line = raw_line.simplified();
        if (!line.isEmpty()) {
            lines.append(line);
        }
    }
    return lines.join('\n').toUtf8().toStdString();
}

std::vector<std::string> split_words(const QString& value) {
    std::vector<std::string> words;
    QString current;
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber()) {
            current.append(ch.toLower());
        } else if (!current.isEmpty()) {
            words.emplace_back(current.toUtf8().toStdString());
            current.clear();
        }
    }
    if (!current.isEmpty()) {
        words.emplace_back(current.toUtf8().toStdString());
    }
    return words;
}

std::string strip_xml_tags(const std::string& xml) {
    std::string output;
    output.reserve(xml.size());
    bool in_tag = false;
    for (char ch : xml) {
        if (ch == '<') {
            in_tag = true;
            continue;
        }
        if (ch == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) {
            output.push_back(ch);
        }
    }
    return output;
}

void replace_all(std::string& value, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = value.find(from.data(), pos, from.size())) != std::string::npos) {
        value.replace(pos, from.size(), to.data(), to.size());
        pos += to.size();
    }
}

std::string decode_basic_entities(std::string value) {
    replace_all(value, "&amp;", "&");
    replace_all(value, "&lt;", "<");
    replace_all(value, "&gt;", ">");
    replace_all(value, "&quot;", "\"");
    replace_all(value, "&apos;", "'");
    return value;
}

#if defined(AI_FILE_SORTER_USE_PUGIXML)
void append_node_text(const pugi::xml_node& node, std::string& out) {
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        out.append(node.value());
        out.push_back(' ');
    }
    for (const auto& child : node.children()) {
        append_node_text(child, out);
    }
}
#endif

std::string extract_xml_text(const std::string& xml) {
#if defined(AI_FILE_SORTER_USE_PUGIXML)
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml.c_str(), pugi::parse_default | pugi::parse_ws_pcdata);
    if (result) {
        std::string out;
        out.reserve(xml.size());
        append_node_text(doc, out);
        return out;
    }
#endif
    return strip_xml_tags(xml);
}

std::optional<std::string> run_process(const QString& program,
                                       const QStringList& args,
                                       int timeout_ms) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted()) {
        return std::nullopt;
    }
    if (!process.waitForFinished(timeout_ms)) {
        process.kill();
        process.waitForFinished();
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return std::nullopt;
    }
    const QByteArray output = process.readAllStandardOutput();
    std::string result(output.constData(), static_cast<size_t>(output.size()));
    if (result.size() > kMaxProcessOutput) {
        result.resize(kMaxProcessOutput);
    }
    return result;
}

std::optional<QString> find_executable(const QString& name) {
    const QString exe = QStandardPaths::findExecutable(name);
    if (exe.isEmpty()) {
        return std::nullopt;
    }
    return exe;
}

#if defined(AI_FILE_SORTER_USE_LIBZIP)
std::optional<std::string> extract_zip_member_libzip(const std::filesystem::path& path,
                                                     std::initializer_list<QString> members) {
    int error_code = 0;
    const std::string archive_path = Utils::path_to_utf8(path);
    zip_t* archive = zip_open(archive_path.c_str(), ZIP_RDONLY, &error_code);
    if (!archive) {
        return std::nullopt;
    }
    for (const auto& member : members) {
        const QByteArray member_name = member.toUtf8();
        zip_file_t* file = zip_fopen(archive, member_name.constData(), 0);
        if (!file) {
            continue;
        }
        std::string output;
        output.reserve(kMaxProcessOutput);
        std::array<char, kZipMemberReadBufferBytes> buffer{};
        zip_int64_t read = 0;
        while ((read = zip_fread(file, buffer.data(), buffer.size())) > 0) {
            output.append(buffer.data(), static_cast<size_t>(read));
            if (output.size() >= kMaxProcessOutput) {
                output.resize(kMaxProcessOutput);
                break;
            }
        }
        zip_fclose(file);
        if (!output.empty()) {
            zip_close(archive);
            return output;
        }
    }
    zip_close(archive);
    return std::nullopt;
}
#endif

std::optional<std::string> extract_zip_member(const std::filesystem::path& path,
                                              std::initializer_list<QString> members,
                                              int timeout_ms)
{
#if defined(AI_FILE_SORTER_USE_LIBZIP)
    if (auto output = extract_zip_member_libzip(path, members)) {
        return output;
    }
#endif
    const auto unzip = find_executable(QStringLiteral("unzip"));
    if (!unzip) {
        return std::nullopt;
    }
    const QString file_path = path_to_qstring(path);
    for (const auto& member : members) {
        if (auto output = run_process(*unzip, {QStringLiteral("-p"), file_path, member}, timeout_ms)) {
            if (!output->empty()) {
                return output;
            }
        }
    }
    return std::nullopt;
}

std::string read_file_prefix(const std::filesystem::path& path, size_t max_chars) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string buffer;
    buffer.resize(max_chars);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(file.gcount()));
    buffer.erase(std::remove(buffer.begin(), buffer.end(), '\0'), buffer.end());
    return buffer;
}

std::string truncate_excerpt(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    const size_t head = max_chars * 3 / 4;
    const size_t tail = max_chars - head;
    return text.substr(0, head) + "\n...\n" + text.substr(text.size() - tail);
}

std::optional<std::string> normalize_date(const std::string& input) {
    std::smatch match;
    static const std::regex kIsoDate(R"((\d{4})-(\d{2})-(\d{2}))");
    if (std::regex_search(input, match, kIsoDate)) {
        return match.str(1) + "-" + match.str(2);
    }
    static const std::regex kPdfDate(R"(D:(\d{4})(\d{2})(\d{2}))");
    if (std::regex_search(input, match, kPdfDate)) {
        return match.str(1) + "-" + match.str(2);
    }
    static const std::regex kCompact(R"(\b(\d{4})(\d{2})(\d{2})\b)");
    if (std::regex_search(input, match, kCompact)) {
        return match.str(1) + "-" + match.str(2);
    }
    return std::nullopt;
}

std::optional<std::string> extract_docx_date(const std::filesystem::path& path) {
    auto xml = extract_zip_member(path, {QStringLiteral("docProps/core.xml")}, 5000);
    if (!xml) {
        return std::nullopt;
    }
    static const std::regex kCreatedTag(R"(<dcterms:created[^>]*>([^<]+)</dcterms:created>)");
    std::smatch match;
    if (std::regex_search(*xml, match, kCreatedTag)) {
        return normalize_date(match.str(1));
    }
    static const std::regex kAltCreatedTag(R"(<cp:created[^>]*>([^<]+)</cp:created>)");
    if (std::regex_search(*xml, match, kAltCreatedTag)) {
        return normalize_date(match.str(1));
    }
    return std::nullopt;
}

std::optional<std::string> clean_metadata_title(std::string title) {
    title = collapse_whitespace(title);
    if (title.empty()) {
        return std::nullopt;
    }

    std::string lower = to_lower_copy(title);
    static const std::array<std::string_view, 4> prefixes = {
        "microsoft word - ",
        "microsoft powerpoint - ",
        "microsoft excel - ",
        "untitled - "
    };
    for (const auto prefix : prefixes) {
        if (lower.rfind(prefix, 0) == 0) {
            title = trim_copy(title.substr(prefix.size()));
            lower = to_lower_copy(title);
            break;
        }
    }

    if (title.empty() || lower == "untitled" || lower == "document" ||
        lower == "unknown" || lower == "title") {
        return std::nullopt;
    }

    const auto dot = title.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        const std::string extension = to_lower_copy(title.substr(dot));
        static const std::unordered_set<std::string> document_extensions = {
            ".pdf", ".docx", ".doc", ".xlsx", ".xls", ".pptx", ".ppt",
            ".odt", ".ods", ".odp", ".txt", ".rtf"
        };
        if (document_extensions.find(extension) != document_extensions.end()) {
            title = trim_copy(title.substr(0, dot));
        }
    }

    return title.empty() ? std::nullopt : std::optional<std::string>(title);
}

std::size_t count_title_words(const std::string& value) {
    std::size_t words = 0;
    bool in_word = false;
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            if (!in_word) {
                ++words;
            }
            in_word = true;
        } else {
            in_word = false;
        }
    }
    return words;
}

std::size_t count_title_letters(const std::string& value) {
    std::size_t letters = 0;
    for (unsigned char ch : value) {
        if (std::isalpha(ch)) {
            ++letters;
        }
    }
    return letters;
}

std::string clean_original_title(std::string value) {
    std::replace(value.begin(), value.end(), '_', ' ');

    static const std::regex kBraced(R"(\{[^}]*\})");
    static const std::regex kBracketed(R"(\[[^\]]*\])");
    static const std::regex kYearParenthetical(R"(\([^)]*\b(?:19|20)\d{2}\b[^)]*\))");
    static const std::regex kDownloadSite(
        R"(\b(?:libgen(?:\.\w+)?|z-lib(?:\.\w+)?|annas-archive(?:\.\w+)?|1lib(?:\.\w+)?)\b)",
        std::regex_constants::icase);

    value = std::regex_replace(value, kBraced, " ");
    value = std::regex_replace(value, kBracketed, " ");
    value = std::regex_replace(value, kYearParenthetical, " ");
    value = std::regex_replace(value, kDownloadSite, " ");
    value = collapse_whitespace(value);
    while (!value.empty() &&
           (value.back() == '.' || value.back() == '-' || value.back() == '_')) {
        value.pop_back();
    }
    return trim_copy(value);
}

bool is_descriptive_original_title(const std::string& title) {
    if (count_title_words(title) < 4 || count_title_letters(title) < 20) {
        return false;
    }

    const std::string lower = to_lower_copy(title);
    static const std::array<std::string_view, 4> weak_prefixes = {
        "untitled", "document", "scan", "file"
    };
    for (const auto prefix : weak_prefixes) {
        if (lower == prefix || lower.rfind(std::string(prefix) + " ", 0) == 0) {
            return false;
        }
    }
    return true;
}

bool is_usable_llm_title(const std::string& title) {
    if (count_title_words(title) < 3 || count_title_letters(title) < 15) {
        return false;
    }
    const std::string lower = to_lower_copy(collapse_whitespace(title));
    static const std::array<std::string_view, 4> weak_titles = {
        "untitled", "document", "unknown", "title"
    };
    for (const auto weak : weak_titles) {
        if (lower == weak) {
            return false;
        }
    }
    return true;
}

std::string choose_document_title(const std::optional<std::string>& metadata_title,
                                  const std::string& original_stem,
                                  const std::string& llm_title,
                                  const std::string& fallback) {
    const std::string original_title = clean_original_title(original_stem);
    const bool descriptive_original = is_descriptive_original_title(original_title);

    if (!metadata_title.has_value() || metadata_title->empty()) {
        if (descriptive_original) {
            return original_title;
        }
        if (is_usable_llm_title(llm_title)) {
            return llm_title;
        }
        return fallback;
    }

    const std::string metadata = collapse_whitespace(*metadata_title);
    if (descriptive_original) {
        const std::size_t metadata_words = count_title_words(metadata);
        const std::size_t original_words = count_title_words(original_title);
        if (metadata_words < 4 || metadata_words + 1 < original_words) {
            return original_title;
        }
    }
    return metadata;
}

std::optional<std::string> extract_docx_title(const std::filesystem::path& path) {
    auto xml = extract_zip_member(path, {QStringLiteral("docProps/core.xml")}, 5000);
    if (!xml) {
        return std::nullopt;
    }
    static const std::regex kTitleTag(R"(<dc:title[^>]*>([^<]*)</dc:title>)");
    std::smatch match;
    if (!std::regex_search(*xml, match, kTitleTag)) {
        return std::nullopt;
    }
    return clean_metadata_title(decode_basic_entities(trim_copy(match.str(1))));
}

std::optional<std::string> extract_odf_title(const std::filesystem::path& path) {
    auto xml = extract_zip_member(path, {QStringLiteral("meta.xml")}, 5000);
    if (!xml) {
        return std::nullopt;
    }
    static const std::regex kTitleTag(R"(<dc:title[^>]*>([^<]*)</dc:title>)");
    std::smatch match;
    if (!std::regex_search(*xml, match, kTitleTag)) {
        return std::nullopt;
    }
    return clean_metadata_title(decode_basic_entities(trim_copy(match.str(1))));
}

std::optional<std::string> extract_pdf_title_via_pdfinfo(const std::filesystem::path& path) {
    const auto pdfinfo = find_executable(QStringLiteral("pdfinfo"));
    if (!pdfinfo) {
        return std::nullopt;
    }
    const auto output = run_process(*pdfinfo, {path_to_qstring(path)}, 4000);
    if (!output) {
        return std::nullopt;
    }
    std::istringstream iss(*output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("Title:", 0) == 0) {
            return clean_metadata_title(line.substr(6));
        }
    }
    return std::nullopt;
}

std::optional<std::string> extract_pdf_date(const std::filesystem::path& path) {
    const QString file_path = path_to_qstring(path);
    if (const auto pdfinfo = find_executable(QStringLiteral("pdfinfo"))) {
        if (auto output = run_process(*pdfinfo, {file_path}, 4000)) {
            std::istringstream iss(*output);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.rfind("CreationDate", 0) == 0 || line.rfind("Creation Date", 0) == 0) {
                    if (auto parsed = normalize_date(line)) {
                        return parsed;
                    }
                }
            }
        }
    }

    std::string raw = read_file_prefix(path, 200000);
    if (raw.empty()) {
        return std::nullopt;
    }
    static const std::regex kCreation(R"(/CreationDate\s*\(([^\)]*)\))");
    std::smatch match;
    if (std::regex_search(raw, match, kCreation)) {
        return normalize_date(match.str(1));
    }
    return std::nullopt;
}

struct EpubMetadata {
    std::string opf_path;
    std::string title;
    std::string author;
    std::string date;
    std::string year;
    std::string publisher;
};

std::string xml_local_name(const char* name) {
    if (name == nullptr) {
        return {};
    }
    std::string value(name);
    const auto colon = value.rfind(':');
    return colon == std::string::npos ? value : value.substr(colon + 1);
}

std::string epub_percent_decode(std::string value) {
    const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hex_value(value[index + 1]);
            const int low = hex_value(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

std::string epub_resolve_path(const std::string& opf_path, std::string href) {
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) {
        return {};
    }
    const auto fragment = href.find_first_of("#?");
    if (fragment != std::string::npos) {
        href.resize(fragment);
    }
    href = epub_percent_decode(href);
    if (href.empty()) {
        return {};
    }

    std::string joined;
    if (href.front() == '/') {
        joined = href.substr(1);
    } else {
        const auto slash = opf_path.find_last_of('/');
        joined = (slash == std::string::npos ? std::string() : opf_path.substr(0, slash + 1)) + href;
    }

    std::vector<std::string> parts;
    std::istringstream stream(joined);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(part);
    }

    std::string resolved;
    for (const auto& component : parts) {
        if (!resolved.empty()) {
            resolved.push_back('/');
        }
        resolved += component;
    }
    return resolved;
}

std::optional<std::string> epub_opf_path(const std::filesystem::path& path) {
    if (auto container = extract_zip_member(path, {QStringLiteral("META-INF/container.xml")}, 5000)) {
        static const std::regex kRootfile(
            R"(<rootfile\b[^>]*full-path\s*=\s*["']([^"']+)["'])",
            std::regex_constants::icase);
        std::smatch match;
        if (std::regex_search(*container, match, kRootfile)) {
            return epub_percent_decode(match.str(1));
        }
    }
    for (const char* candidate : {"OEBPS/content.opf", "content.opf", "EPUB/content.opf", "OPS/content.opf"}) {
        if (auto opf = extract_zip_member(path, {QString::fromUtf8(candidate)}, 5000)) {
            return std::string(candidate);
        }
    }
    return std::nullopt;
}

#if defined(AI_FILE_SORTER_USE_PUGIXML)
void collect_xml_nodes_by_name(const pugi::xml_node& node,
                               std::string_view name,
                               std::vector<std::string>& out) {
    if (node.type() == pugi::node_element && xml_local_name(node.name()) == name) {
        const std::string text = node.text().get();
        if (!text.empty()) {
            out.push_back(text);
        }
    }
    for (const auto& child : node.children()) {
        collect_xml_nodes_by_name(child, name, out);
    }
}

pugi::xml_node find_xml_node(const pugi::xml_node& node, std::string_view name) {
    if (node.type() == pugi::node_element && xml_local_name(node.name()) == name) {
        return node;
    }
    for (const auto& child : node.children()) {
        if (auto found = find_xml_node(child, name)) {
            return found;
        }
    }
    return {};
}
#endif

std::vector<std::string> epub_metadata_values(const std::string& opf, std::string_view field) {
    std::vector<std::string> values;
#if defined(AI_FILE_SORTER_USE_PUGIXML)
    pugi::xml_document doc;
    if (doc.load_string(opf.c_str(), pugi::parse_default | pugi::parse_ws_pcdata)) {
        collect_xml_nodes_by_name(doc, field, values);
    }
#endif
    if (!values.empty()) {
        return values;
    }

    const std::string field_name(field);
    const std::regex pattern("<(?:[A-Za-z0-9_]+:)?" + field_name +
                                 "\\b[^>]*>([^<]*)</(?:[A-Za-z0-9_]+:)?" + field_name + ">",
                             std::regex_constants::icase);
    for (std::sregex_iterator it(opf.begin(), opf.end(), pattern), end; it != end; ++it) {
        values.push_back((*it).str(1));
    }
    return values;
}

EpubMetadata extract_epub_metadata(const std::filesystem::path& path) {
    EpubMetadata metadata;
    const auto opf_path = epub_opf_path(path);
    if (!opf_path) {
        return metadata;
    }
    const auto opf = extract_zip_member(path, {QString::fromStdString(*opf_path)}, 10000);
    if (!opf) {
        return metadata;
    }
    metadata.opf_path = *opf_path;

    for (const auto& value : epub_metadata_values(*opf, "title")) {
        if (auto cleaned = clean_metadata_title(decode_basic_entities(collapse_whitespace(value)))) {
            metadata.title = *cleaned;
            break;
        }
    }

    for (const auto& value : epub_metadata_values(*opf, "creator")) {
        const std::string creator = collapse_whitespace(decode_basic_entities(value));
        if (creator.empty()) {
            continue;
        }
        if (!metadata.author.empty()) {
            metadata.author += "; ";
        }
        metadata.author += creator;
    }

    for (const auto& value : epub_metadata_values(*opf, "date")) {
        const std::string raw = collapse_whitespace(value);
        if (raw.empty()) {
            continue;
        }
        if (auto normalized = normalize_date(raw)) {
            metadata.date = *normalized;
            metadata.year = normalized->substr(0, 4);
            break;
        }
        static const std::regex kYear(R"(\b((?:19|20)\d{2})\b)");
        std::smatch match;
        if (std::regex_search(raw, match, kYear)) {
            metadata.year = match.str(1);
            break;
        }
    }

    for (const auto& value : epub_metadata_values(*opf, "publisher")) {
        const std::string publisher = collapse_whitespace(decode_basic_entities(value));
        if (!publisher.empty()) {
            metadata.publisher = publisher;
            break;
        }
    }
    return metadata;
}

std::vector<std::string> epub_section_paths(const std::string& opf,
                                            const std::string& opf_path) {
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;

#if defined(AI_FILE_SORTER_USE_PUGIXML)
    pugi::xml_document doc;
    if (doc.load_string(opf.c_str(), pugi::parse_default | pugi::parse_ws_pcdata)) {
        std::unordered_map<std::string, std::string> manifest;
        const auto manifest_node = find_xml_node(doc, "manifest");
        for (const auto& item : manifest_node.children("item")) {
            const std::string id = item.attribute("id").value();
            const std::string href = item.attribute("href").value();
            if (!id.empty() && !href.empty()) {
                manifest.emplace(id, epub_resolve_path(opf_path, href));
            }
        }

        const auto spine = find_xml_node(doc, "spine");
        for (const auto& itemref : spine.children("itemref")) {
            const std::string idref = itemref.attribute("idref").value();
            const auto it = manifest.find(idref);
            if (it != manifest.end() && !it->second.empty() && seen.insert(it->second).second) {
                paths.push_back(it->second);
            }
        }

        if (paths.empty()) {
            for (const auto& item : manifest_node.children("item")) {
                const std::string media_type = item.attribute("media-type").value();
                const std::string href = item.attribute("href").value();
                if (media_type.find("xhtml") == std::string::npos || href.empty()) {
                    continue;
                }
                const std::string resolved = epub_resolve_path(opf_path, href);
                if (!resolved.empty() && seen.insert(resolved).second) {
                    paths.push_back(resolved);
                }
            }
        }
    }
#endif

    if (paths.empty()) {
        static const std::regex kItem(
            R"(<item\b[^>]*\bhref\s*=\s*["']([^"']+)["'][^>]*>)",
            std::regex_constants::icase);
        for (std::sregex_iterator it(opf.begin(), opf.end(), kItem), end; it != end; ++it) {
            const std::string tag = (*it).str(0);
            if (tag.find("xhtml") == std::string::npos && tag.find("html") == std::string::npos) {
                continue;
            }
            const std::string resolved = epub_resolve_path(opf_path, (*it).str(1));
            if (!resolved.empty() && seen.insert(resolved).second) {
                paths.push_back(resolved);
            }
        }
    }
    return paths;
}

#if defined(AI_FILE_SORTER_USE_PUGIXML)
void append_epub_text(const pugi::xml_node& node, std::string& out) {
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        out.append(node.value());
        out.push_back(' ');
        return;
    }
    const std::string name = xml_local_name(node.name());
    if (name == "head" || name == "script" || name == "style") {
        return;
    }
    for (const auto& child : node.children()) {
        append_epub_text(child, out);
    }
}
#endif

std::string extract_epub_xhtml_text(const std::string& xml) {
#if defined(AI_FILE_SORTER_USE_PUGIXML)
    pugi::xml_document doc;
    if (doc.load_string(xml.c_str(), pugi::parse_default | pugi::parse_ws_pcdata)) {
        std::string out;
        append_epub_text(doc, out);
        return collapse_whitespace(decode_basic_entities(out));
    }
#endif
    return collapse_whitespace(decode_basic_entities(strip_xml_tags(xml)));
}

std::vector<PdfPageText> extract_epub_sections(const std::filesystem::path& path,
                                               int max_sections) {
    std::vector<PdfPageText> sections;
    const auto opf_path = epub_opf_path(path);
    if (!opf_path) {
        return sections;
    }
    const auto opf = extract_zip_member(path, {QString::fromStdString(*opf_path)}, 10000);
    if (!opf) {
        return sections;
    }

    for (const std::string& section_path : epub_section_paths(*opf, *opf_path)) {
        if (static_cast<int>(sections.size()) >= max_sections) {
            break;
        }
        const auto section =
            extract_zip_member(path, {QString::fromStdString(section_path)}, 10000);
        if (!section) {
            continue;
        }
        const std::string text = extract_epub_xhtml_text(*section);
        if (text.empty()) {
            continue;
        }
        const std::string cleaned = clean_pdf_page_text(
            QString::fromUtf8(text.c_str(), static_cast<qsizetype>(text.size())));
        if (!cleaned.empty()) {
            sections.push_back(PdfPageText{static_cast<int>(sections.size()) + 1, cleaned});
        }
    }
    return sections;
}

std::string extract_epub_text(const std::filesystem::path& path, size_t max_chars) {
    std::string text;
    for (const auto& section : extract_epub_sections(path, kPdfNameMaxPages)) {
        if (text.size() >= max_chars) {
            break;
        }
        text += section.text;
        text.push_back('\n');
    }
    if (text.size() > max_chars) {
        text.resize(max_chars);
    }
    return collapse_whitespace(text);
}

std::optional<std::string> extract_epub_title(const std::filesystem::path& path) {
    const EpubMetadata metadata = extract_epub_metadata(path);
    return metadata.title.empty() ? std::nullopt : std::optional<std::string>(metadata.title);
}

std::optional<std::string> extract_epub_date(const std::filesystem::path& path) {
    const EpubMetadata metadata = extract_epub_metadata(path);
    return metadata.date.empty() ? std::nullopt : std::optional<std::string>(metadata.date);
}

#if defined(AI_FILE_SORTER_USE_PDFIUM)
class PdfiumLibraryGuard {
public:
    PdfiumLibraryGuard() { FPDF_InitLibrary(); }
    ~PdfiumLibraryGuard() { FPDF_DestroyLibrary(); }
};

PdfiumLibraryGuard& pdfium_library() {
    static PdfiumLibraryGuard guard;
    return guard;
}

std::string extract_pdf_text_pdfium(const std::filesystem::path& path, size_t max_chars) {
    pdfium_library();
    const std::string pdf_path = Utils::path_to_utf8(path);
    FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!doc) {
        return {};
    }
    const int page_count = FPDF_GetPageCount(doc);
    std::string result;
    for (int i = 0; i < page_count && result.size() < max_chars; ++i) {
        FPDF_PAGE page = FPDF_LoadPage(doc, i);
        if (!page) {
            continue;
        }
        FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
        if (!text_page) {
            FPDF_ClosePage(page);
            continue;
        }
        const int total_chars = FPDFText_CountChars(text_page);
        const int chunk = kPdfiumTextChunkChars;
        int offset = 0;
        while (offset < total_chars && result.size() < max_chars) {
            const int take = std::min(chunk, total_chars - offset);
            std::vector<unsigned short> buffer(static_cast<size_t>(take + 1));
            const int extracted = FPDFText_GetText(text_page, offset, take, buffer.data());
            if (extracted <= 0) {
                break;
            }
            const int count = std::max(0, extracted - 1);
            QString text = QString::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), count);
            const QByteArray bytes = text.toUtf8();
            result.append(bytes.constData(), static_cast<size_t>(bytes.size()));
            if (result.size() >= max_chars) {
                result.resize(max_chars);
                break;
            }
            offset += take;
        }
        FPDFText_ClosePage(text_page);
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(doc);
    return result;
}

std::string extract_pdf_title_pdfium(const std::filesystem::path& path) {
    pdfium_library();
    const std::string pdf_path = Utils::path_to_utf8(path);
    FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!doc) {
        return {};
    }

    const unsigned long byte_length = FPDF_GetMetaText(doc, "Title", nullptr, 0);
    if (byte_length <= 2) {
        FPDF_CloseDocument(doc);
        return {};
    }

    std::vector<unsigned short> buffer((byte_length + 1) / 2 + 1, 0);
    const unsigned long copied = FPDF_GetMetaText(doc, "Title", buffer.data(), byte_length);
    FPDF_CloseDocument(doc);
    if (copied <= 2) {
        return {};
    }

    QString text = QString::fromUtf16(
        reinterpret_cast<const char16_t*>(buffer.data()),
        static_cast<int>(copied / 2));
    text.remove(QChar(0));
    return text.toUtf8().toStdString();
}

std::string extract_pdf_metadata_author_pdfium(const std::filesystem::path& path) {
    pdfium_library();
    const std::string pdf_path = Utils::path_to_utf8(path);
    FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!doc) {
        return {};
    }

    const unsigned long byte_length = FPDF_GetMetaText(doc, "Author", nullptr, 0);
    if (byte_length <= 2) {
        FPDF_CloseDocument(doc);
        return {};
    }

    std::vector<unsigned short> buffer((byte_length + 1) / 2 + 1, 0);
    const unsigned long copied = FPDF_GetMetaText(doc, "Author", buffer.data(), byte_length);
    FPDF_CloseDocument(doc);
    if (copied <= 2) {
        return {};
    }

    QString text = QString::fromUtf16(
        reinterpret_cast<const char16_t*>(buffer.data()),
        static_cast<int>(copied / 2));
    text.remove(QChar(0));
    return text.toUtf8().toStdString();
}

std::vector<PdfPageText> extract_pdf_pages_pdfium(const std::filesystem::path& path,
                                                  int max_pages) {
    pdfium_library();
    const std::string pdf_path = Utils::path_to_utf8(path);
    FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!doc) {
        return {};
    }

    std::vector<PdfPageText> pages;
    const int page_count = FPDF_GetPageCount(doc);
    for (int index = 0; index < page_count && index < max_pages; ++index) {
        FPDF_PAGE page = FPDF_LoadPage(doc, index);
        if (!page) {
            continue;
        }
        FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
        if (!text_page) {
            FPDF_ClosePage(page);
            continue;
        }

        QString text;
        const int total_chars = FPDFText_CountChars(text_page);
        int offset = 0;
        while (offset < total_chars) {
            const int take = std::min(kPdfiumTextChunkChars, total_chars - offset);
            std::vector<unsigned short> buffer(static_cast<size_t>(take + 1));
            const int extracted = FPDFText_GetText(text_page, offset, take, buffer.data());
            if (extracted <= 0) {
                break;
            }
            const int count = std::max(0, extracted - 1);
            text.append(QString::fromUtf16(
                reinterpret_cast<const char16_t*>(buffer.data()), count));
            offset += take;
        }

        FPDFText_ClosePage(text_page);
        FPDF_ClosePage(page);
        const std::string cleaned = clean_pdf_page_text(text);
        if (!cleaned.empty()) {
            pages.push_back(PdfPageText{index + 1, cleaned});
        }
    }

    FPDF_CloseDocument(doc);
    return pages;
}
#endif

std::vector<PdfPageText> extract_pdf_pages_fallback(const std::filesystem::path& path,
                                                    int max_pages) {
    const auto pdftotext = find_executable(QStringLiteral("pdftotext"));
    if (!pdftotext) {
        return {};
    }
    const auto output = run_process(
        *pdftotext,
        {QStringLiteral("-layout"), QStringLiteral("-q"), QStringLiteral("-f"),
         QStringLiteral("1"), QStringLiteral("-l"), QString::number(max_pages),
         path_to_qstring(path), QStringLiteral("-")},
        15000);
    if (!output) {
        return {};
    }

    std::vector<PdfPageText> pages;
    const QStringList chunks = QString::fromUtf8(output->c_str()).split(QChar('\f'));
    for (int index = 0; index < chunks.size() && index < max_pages; ++index) {
        const std::string cleaned = clean_pdf_page_text(chunks.at(index));
        if (!cleaned.empty()) {
            pages.push_back(PdfPageText{index + 1, cleaned});
        }
    }
    return pages;
}

std::vector<PdfPageText> extract_pdf_pages(const std::filesystem::path& path, int max_pages) {
#if defined(AI_FILE_SORTER_USE_PDFIUM)
    if (auto pages = extract_pdf_pages_pdfium(path, max_pages); !pages.empty()) {
        return pages;
    }
#endif
    return extract_pdf_pages_fallback(path, max_pages);
}

std::string extract_pdf_author(const std::filesystem::path& path) {
#if defined(AI_FILE_SORTER_USE_PDFIUM)
    if (auto author = extract_pdf_metadata_author_pdfium(path); !author.empty()) {
        return author;
    }
#endif
    const auto pdfinfo = find_executable(QStringLiteral("pdfinfo"));
    if (!pdfinfo) {
        return {};
    }
    const auto output = run_process(*pdfinfo, {path_to_qstring(path)}, 4000);
    if (!output) {
        return {};
    }
    std::istringstream iss(*output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("Author:", 0) == 0) {
            return collapse_whitespace(line.substr(7));
        }
    }
    return {};
}

QString pdf_qstring(const std::string& value) {
    return QString::fromUtf8(value.c_str(), static_cast<qsizetype>(value.size()));
}

QString pdf_compact(const QString& value) {
    QString normalized = value.normalized(QString::NormalizationForm_KC).toCaseFolded();
    QString compact;
    compact.reserve(normalized.size());
    for (const QChar ch : normalized) {
        if (ch.isLetterOrNumber()) {
            compact.append(ch);
        }
    }
    return compact;
}

bool pdf_exact_span(const QString& value, const QString& evidence, int min_chars = 5) {
    const QString needle = pdf_compact(value);
    return needle.size() >= min_chars && pdf_compact(evidence).contains(needle);
}

int pdf_word_count(const QString& value) {
    return value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
}

QStringList pdf_tokens(const QString& value) {
    QStringList parts = value.split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")),
                                    Qt::SkipEmptyParts);
    QStringList filtered;
    for (const QString& part : parts) {
        if (part.size() > 1) {
            filtered.append(part.toCaseFolded());
        }
    }
    return filtered;
}

bool pdf_forbidden_title(const QString& value) {
    const QString lowered = value.trimmed().toCaseFolded();
    if (lowered.contains(QStringLiteral("for further volumes")) ||
        lowered.contains(QStringLiteral("http://")) ||
        lowered.contains(QStringLiteral("https://")) ||
        lowered.contains(QStringLiteral("www.")) ||
        lowered.contains(QStringLiteral("springer.com/series"))) {
        return true;
    }
    if (QRegularExpression(
            QStringLiteral("^lecture notes in .+\\bvolume\\s+\\d+\\b"),
            QRegularExpression::CaseInsensitiveOption)
            .match(lowered)
            .hasMatch()) {
        return true;
    }
    static const QStringList prefixes = {
        QStringLiteral("praise for"),
        QStringLiteral("a note"),
        QStringLiteral("note for"),
        QStringLiteral("copyright"),
        QStringLiteral("contents"),
        QStringLiteral("table of contents"),
        QStringLiteral("about the author"),
        QStringLiteral("this book"),
        QStringLiteral("preface"),
        QStringLiteral("aims"),
        QStringLiteral("structure of the book"),
        QStringLiteral("chapter"),
        QStringLiteral("introduction"),
    };
    for (const QString& prefix : prefixes) {
        if (lowered == prefix || lowered.startsWith(prefix + QLatin1Char(' '))) {
            return true;
        }
    }
    return false;
}

bool pdf_title_looks_like_prose(const QString& value) {
    if (pdf_word_count(value) > 30 || value.size() > 220) {
        return true;
    }
    if (pdf_forbidden_title(value)) {
        return true;
    }
    if (QRegularExpression(QStringLiteral("[.!?]\\s+[A-Z]")).match(value).hasMatch()) {
        return true;
    }
    return QRegularExpression(
               QStringLiteral("\\b(has the aims|aims to|is designed to|is intended to)\\b"),
               QRegularExpression::CaseInsensitiveOption)
        .match(value)
        .hasMatch();
}

QStringList pdf_running_header_candidates(const std::vector<PdfPageText>& pages) {
    QStringList candidates;
    QSet<QString> seen;
    const QRegularExpression header(QStringLiteral("^(\\d{1,3})\\s+(.+)$"));
    for (const auto& page : pages) {
        const QStringList lines = pdf_qstring(page.text).split(QLatin1Char('\n'));
        for (const QString& raw_line : lines) {
            const QString line = raw_line.simplified();
            const auto match = header.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            const QString candidate = match.captured(2).trimmed();
            const QStringList words = candidate.split(QRegularExpression(QStringLiteral("\\s+")),
                                                      Qt::SkipEmptyParts);
            int capitalized = 0;
            for (const QString& word : words) {
                if (!word.isEmpty() && word.front().isUpper()) {
                    ++capitalized;
                }
            }
            if (words.size() < 2 || words.size() > 12 || capitalized < 2 ||
                candidate.size() < 4 || candidate.size() > 90 ||
                pdf_title_looks_like_prose(candidate)) {
                continue;
            }
            const QString key = pdf_compact(candidate);
            if (!key.isEmpty() && !seen.contains(key)) {
                seen.insert(key);
                candidates.append(candidate);
            }
        }
    }
    return candidates.mid(0, 5);
}

QString pdf_find_edition(const QString& evidence) {
    const auto numeric = QRegularExpression(
                             QStringLiteral("\\b(\\d{1,2}(?:st|nd|rd|th)\\s+edition)\\b"),
                             QRegularExpression::CaseInsensitiveOption)
                             .match(evidence);
    if (numeric.hasMatch()) {
        return numeric.captured(1).simplified();
    }
    const auto numbered = QRegularExpression(
                              QStringLiteral("\\b(Edition\\s+\\d+(?:\\.\\d+)?)\\b"),
                              QRegularExpression::CaseInsensitiveOption)
                              .match(evidence);
    if (numbered.hasMatch()) {
        return numbered.captured(1).simplified();
    }
    static const QStringList words = {
        QStringLiteral("First"), QStringLiteral("Second"), QStringLiteral("Third"),
        QStringLiteral("Fourth"), QStringLiteral("Fifth"), QStringLiteral("Sixth"),
        QStringLiteral("Seventh"), QStringLiteral("Eighth"), QStringLiteral("Ninth"),
        QStringLiteral("Tenth"), QStringLiteral("Eleventh"), QStringLiteral("Twelfth"),
    };
    const QString word_pattern = words.join(QLatin1Char('|'));
    const auto title_page = QRegularExpression(
                                QStringLiteral("\\b((?:%1)\\s+Edition)\\b").arg(word_pattern))
                                .match(evidence);
    if (title_page.hasMatch()) {
        return title_page.captured(1);
    }
    const auto loose = QRegularExpression(
                           QStringLiteral("\\b((?:%1)\\s+edition)\\b").arg(word_pattern),
                           QRegularExpression::CaseInsensitiveOption)
                           .match(evidence);
    if (loose.hasMatch()) {
        QStringList parts = loose.captured(1).split(QLatin1Char(' '));
        for (QString& part : parts) {
            part[0] = part[0].toUpper();
            part = part.toLower();
            part[0] = part[0].toUpper();
        }
        return parts.join(QLatin1Char(' '));
    }
    return {};
}

bool pdf_year_has_context(const QString& year, const QString& evidence) {
    if (!QRegularExpression(QStringLiteral("^(?:19|20)\\d{2}$")).match(year).hasMatch()) {
        return false;
    }
    const QRegularExpression years(QStringLiteral("\\b(?:19|20)\\d{2}\\b"));
    auto it = years.globalMatch(evidence);
    while (it.hasNext()) {
        const auto match = it.next();
        if (match.captured(0) != year) {
            continue;
        }
        const int start = std::max(0, static_cast<int>(match.capturedStart()) - 90);
        const QString window = evidence.mid(start, 180).toCaseFolded();
        if (window.contains(QStringLiteral("published")) ||
            window.contains(QStringLiteral("copyright")) ||
            window.contains(QStringLiteral("©")) ||
            window.contains(QStringLiteral("edition")) ||
            window.contains(QStringLiteral("printing")) ||
            window.contains(QStringLiteral("isbn"))) {
            return true;
        }
    }
    return false;
}

bool pdf_author_list_text(const QString& value, const QStringList& authors) {
    static const QSet<QString> connectors = {
        QStringLiteral("and"), QStringLiteral("y"), QStringLiteral("e"),
        QStringLiteral("et"), QStringLiteral("with"), QStringLiteral("by"),
    };
    QStringList value_tokens;
    for (const QString& token : pdf_tokens(value)) {
        if (!connectors.contains(token)) {
            value_tokens.append(token);
        }
    }
    QSet<QString> author_tokens;
    for (const QString& author : authors) {
        for (const QString& token : pdf_tokens(author)) {
            author_tokens.insert(token);
        }
    }
    if (value_tokens.isEmpty()) {
        return false;
    }
    for (const QString& token : value_tokens) {
        if (!author_tokens.contains(token)) {
            return false;
        }
    }
    return true;
}

QString pdf_normalise_report_date(const QString& value) {
    const auto match = QRegularExpression(
                           QStringLiteral("^(\\d{1,2})/([A-Za-z]{3})/(\\d{4})$"))
                           .match(value.trimmed());
    if (!match.hasMatch()) {
        return {};
    }
    static const QStringList months = {
        QStringLiteral("ene"), QStringLiteral("feb"), QStringLiteral("mar"),
        QStringLiteral("abr"), QStringLiteral("may"), QStringLiteral("jun"),
        QStringLiteral("jul"), QStringLiteral("ago"), QStringLiteral("sep"),
        QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dic"),
    };
    const int month = months.indexOf(match.captured(2).toCaseFolded()) + 1;
    if (month <= 0) {
        return {};
    }
    return QStringLiteral("%1-%2-%3")
        .arg(match.captured(3))
        .arg(month, 2, 10, QLatin1Char('0'))
        .arg(match.captured(1).toInt(), 2, 10, QLatin1Char('0'));
}

QString pdf_medical_name(const QString& evidence, const std::filesystem::path& original) {
    const auto flags = QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption;
    if (!QRegularExpression(QStringLiteral("^\\s*AN[ÁA]LISIS DE SANGRE\\s*$"), flags)
             .match(evidence)
             .hasMatch()) {
        return {};
    }
    const auto patient = QRegularExpression(QStringLiteral("^\\s*Paciente:\\s*(.+?)\\s*$"), flags)
                             .match(evidence);
    const auto date = QRegularExpression(
                          QStringLiteral("^\\s*(?:F\\.\\s*extracci[oó]n|Fecha):\\s*(\\d{1,2}/[A-Za-z]{3}/\\d{4})"),
                          flags)
                          .match(evidence);
    if (!patient.hasMatch() || !date.hasMatch()) {
        return {};
    }
    const QString normalized_date = pdf_normalise_report_date(date.captured(1));
    if (normalized_date.isEmpty()) {
        return {};
    }
    const auto number = QRegularExpression(QStringLiteral("^\\s*An[aá]lisis:\\s*(\\d+)"), flags)
                            .match(evidence);
    QString stem = QStringLiteral("Analisis de sangre - %1 - %2")
                       .arg(patient.captured(1).simplified(), normalized_date);
    if (number.hasMatch()) {
        stem += QStringLiteral(" - ") + number.captured(1);
    }
    stem.replace(QRegularExpression(QStringLiteral("[\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral(" "));
    stem = stem.simplified();
    return stem + QString::fromStdString(Utils::path_to_utf8(original.extension()));
}

QString pdf_compose_name(const QString& title,
                         const QString& subtitle,
                         const QStringList& authors,
                         const QString& year,
                         const QString& edition,
                         const std::filesystem::path& original) {
    if (title.isEmpty()) {
        return {};
    }
    QString title_text = title.trimmed();
    if (!edition.isEmpty()) {
        title_text += QStringLiteral(" (") + edition + QLatin1Char(')');
    }
    if (!subtitle.isEmpty() && !pdf_compact(title_text).contains(pdf_compact(subtitle))) {
        title_text += QStringLiteral(": ") + subtitle;
    }
    title_text.replace(QLatin1Char(':'), QStringLiteral(" - "));
    title_text.replace(QLatin1Char('/'), QStringLiteral(" - "));
    title_text.replace(QLatin1Char('\\'), QStringLiteral(" - "));
    title_text.replace(QRegularExpression(QStringLiteral("[*?\"<>|\\x00-\\x1f]")), QStringLiteral(" "));
    title_text = title_text.simplified().remove(QRegularExpression(QStringLiteral("^[ ._-]+|[ ._-]+$")));

    QString suffix;
    if (!authors.isEmpty()) {
        suffix = QStringLiteral(" - ") + authors.join(QStringLiteral(", "));
    }
    if (!year.isEmpty()) {
        suffix += QStringLiteral(" (") + year + QLatin1Char(')');
    }

    constexpr int kMaxStem = 180;
    if (title_text.size() + suffix.size() > kMaxStem) {
        const int budget = std::max(20, kMaxStem - static_cast<int>(suffix.size()));
        int cut = budget;
        while (cut > 0 && cut < title_text.size() && !title_text.at(cut).isSpace()) {
            --cut;
        }
        title_text = title_text.left(cut).trimmed();
    }
    const QString stem = (title_text + suffix).trimmed();
    return stem + QString::fromStdString(Utils::path_to_utf8(original.extension()));
}

QString pdf_build_evidence(const std::vector<PdfPageText>& pages,
                           const QString& metadata_title,
                           const QString& metadata_author,
                           int budget) {
    QString evidence = QStringLiteral(
        "PDF METADATA (may be incomplete or wrong; prefer the title page):\nTitle: %1\nAuthor: %2\n")
        .arg(metadata_title.isEmpty() ? QStringLiteral("(empty)") : metadata_title,
             metadata_author.isEmpty() ? QStringLiteral("(empty)") : metadata_author);
    const QStringList candidates = pdf_running_header_candidates(pages);
    if (!candidates.isEmpty()) {
        evidence += QStringLiteral("\nPOSSIBLE BOOK TITLE LINES (from running headers; verify):\n");
        for (const QString& candidate : candidates) {
            evidence += QStringLiteral("- ") + candidate + QLatin1Char('\n');
        }
    }
    evidence += QStringLiteral("\nTITLE PAGES / FIRST PAGES:\n");

    const int per_page = std::max(kPdfNameMinPageChars, budget / kPdfNameMaxPages);
    int used = evidence.size();
    for (const auto& page : pages) {
        QString page_text = pdf_qstring(page.text);
        if (page_text.size() > per_page) {
            page_text = page_text.left(per_page);
        }
        const QString block = QStringLiteral("\n--- PDF PAGE %1 ---\n%2\n")
                                  .arg(page.page)
                                  .arg(page_text);
        if (used + block.size() > budget) {
            const int remaining = budget - used;
            if (remaining > 500) {
                evidence += block.left(remaining);
            }
            break;
        }
        evidence += block;
        used += block.size();
    }
    return evidence;
}

QString epub_build_evidence(const EpubMetadata& metadata,
                            const std::vector<PdfPageText>& sections,
                            int budget) {
    QString evidence = QStringLiteral(
        "EPUB METADATA (from the OPF package; may be incomplete or wrong; prefer the title page):\nTitle: %1\nAuthor: %2\nPublished: %3\nPublisher: %4\n")
        .arg(metadata.title.empty() ? QStringLiteral("(empty)") : pdf_qstring(metadata.title))
        .arg(metadata.author.empty() ? QStringLiteral("(empty)") : pdf_qstring(metadata.author))
        .arg(metadata.date.empty()
                 ? (metadata.year.empty() ? QStringLiteral("(empty)") : pdf_qstring(metadata.year))
                 : pdf_qstring(metadata.date))
        .arg(metadata.publisher.empty() ? QStringLiteral("(empty)") : pdf_qstring(metadata.publisher));
    evidence += QStringLiteral("\nEPUB SECTIONS / TITLE PAGES:\n");

    const int per_section = std::max(kPdfNameMinPageChars, budget / kPdfNameMaxPages);
    int used = evidence.size();
    for (const auto& section : sections) {
        QString section_text = pdf_qstring(section.text);
        if (section_text.size() > per_section) {
            section_text = section_text.left(per_section);
        }
        const QString block = QStringLiteral("\n--- EPUB SECTION %1 ---\n%2\n")
                                  .arg(section.page)
                                  .arg(section_text);
        if (used + block.size() > budget) {
            const int remaining = budget - used;
            if (remaining > 500) {
                evidence += block.left(remaining);
            }
            break;
        }
        evidence += block;
        used += block.size();
    }
    return evidence;
}

QString pdf_build_prompt() {
    return QStringLiteral(
        "You are a bibliographic metadata extractor. You copy evidence; you never guess, "
        "summarize, translate, or invent.\n\n"
        "Return one JSON object with exactly these keys:\n"
        "{\"title\": string or null, \"subtitle\": string or null, \"authors\": array of strings, "
        "\"year\": string or null, \"publisher\": string or null, \"confidence\": \"high\"|\"medium\"|\"low\"}\n\n"
        "Rules:\n"
        "- The input contains no filename. Do not infer anything from a filename.\n"
        "- Use the title page when there is one. Prefer the fullest title printed there over embedded metadata.\n"
        "- A series page such as `Lecture Notes in ... Volume N`, `For further volumes:`, a publisher URL, or a series ISSN is not a title page. Never use its series name, volume number, URL, or `For further volumes` as title or subtitle.\n"
        "- Prefer a page where the book title appears together with the authors over a series, copyright, or metadata page.\n"
        "- If the first pages are a preface, introduction, or chapter and there is no title page, do not use the preface text or its first sentence as the title.\n"
        "- Look for a short standalone title or a running header such as `2 Instrumentation and Control Systems`; the title is the part after the page number. Prefer it over a chapter heading such as `1 Measurement systems`.\n"
        "- A title is a short noun phrase, not a sentence or a paragraph. Never use text starting with `This book...`.\n"
        "- \"title\" and \"subtitle\" must be copied verbatim from the evidence, preserving spelling, accents, punctuation, capitalization, and original language.\n"
        "- Do not translate, title-case, or add words.\n"
        "- Split a printed title at a colon or line break when that gives a clean title and subtitle.\n"
        "- A subtitle is a short secondary title, never a marketing description, blurb, author list, or edition statement.\n"
        "- Authors must be names printed on the title page, in printed order. If absent, use the document's embedded metadata author. Never add an author not in the evidence.\n"
        "- year/publisher only when present in the evidence; otherwise null.\n"
        "- If no exact title is visible, set title to null. A low-confidence null is better than a plausible invention.\n"
        "- Return JSON only, with no explanation or markdown.\n");
}

struct ParsedPdfName {
    QString title;
    QString subtitle;
    QString year;
    QString publisher;
    QString confidence;
    QStringList authors;
};

std::optional<ParsedPdfName> parse_pdf_name_json(const std::string& response) {
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;
    auto parse = [&](const std::string& payload) {
        std::istringstream stream(payload);
        return Json::parseFromStream(reader, stream, &root, &errors);
    };
    if (!parse(response)) {
        const auto first = response.find('{');
        const auto last = response.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first ||
            !parse(response.substr(first, last - first + 1))) {
            return std::nullopt;
        }
    }
    if (!root.isObject()) {
        return std::nullopt;
    }
    ParsedPdfName parsed;
    parsed.title = pdf_qstring(root["title"].asString()).trimmed();
    parsed.subtitle = pdf_qstring(root["subtitle"].asString()).trimmed();
    parsed.year = pdf_qstring(root["year"].asString()).trimmed();
    parsed.publisher = pdf_qstring(root["publisher"].asString()).trimmed();
    parsed.confidence = pdf_qstring(root["confidence"].asString()).trimmed();
    if (root["authors"].isArray()) {
        for (const auto& author : root["authors"]) {
            const QString value = pdf_qstring(author.asString()).trimmed();
            if (!value.isEmpty()) {
                parsed.authors.append(value);
            }
        }
    }
    return parsed;
}

QString pdf_choose_name(const ParsedPdfName& parsed,
                        const QString& evidence,
                        const QString& metadata_title,
                        const QString& metadata_author,
                        const std::vector<PdfPageText>& pages,
                        const std::filesystem::path& original) {
    QStringList authors;
    QSet<QString> seen_authors;
    for (const QString& author : parsed.authors) {
        if (pdf_exact_span(author, evidence, 5) && !seen_authors.contains(pdf_compact(author))) {
            seen_authors.insert(pdf_compact(author));
            authors.append(author.simplified());
        }
    }
    if (authors.isEmpty() && !metadata_author.isEmpty()) {
        const QStringList parts = metadata_author.split(
            QRegularExpression(QStringLiteral("\\s*[;&]\\s*|\\s+and\\s+")), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            if (pdf_exact_span(part, evidence, 5) && !seen_authors.contains(pdf_compact(part))) {
                seen_authors.insert(pdf_compact(part));
                authors.append(part.simplified());
            }
        }
    }

    QString title;
    if (!parsed.title.isEmpty() && !pdf_forbidden_title(parsed.title) &&
        !pdf_title_looks_like_prose(parsed.title) && pdf_exact_span(parsed.title, evidence)) {
        title = parsed.title;
    }
    if (title.isEmpty() && !metadata_title.isEmpty() &&
        !pdf_forbidden_title(metadata_title) && pdf_exact_span(metadata_title, evidence)) {
        title = metadata_title;
    }
    if (title.isEmpty()) {
        for (const QString& candidate : pdf_running_header_candidates(pages)) {
            if (!pdf_title_looks_like_prose(candidate) && pdf_exact_span(candidate, evidence)) {
                title = candidate;
                break;
            }
        }
    }
    if (title.isEmpty()) {
        return {};
    }

    const QString edition = pdf_find_edition(evidence);
    QString subtitle;
    if (!parsed.subtitle.isEmpty() && pdf_exact_span(parsed.subtitle, evidence) &&
        pdf_word_count(parsed.subtitle) <= 14 && parsed.subtitle.size() <= 120 &&
        !pdf_forbidden_title(parsed.subtitle) && !pdf_author_list_text(parsed.subtitle, authors) &&
        (edition.isEmpty() || pdf_compact(parsed.subtitle) != pdf_compact(edition)) &&
        !pdf_compact(title).contains(pdf_compact(parsed.subtitle))) {
        subtitle = parsed.subtitle.simplified();
    }

    QString year = parsed.year;
    if (!pdf_year_has_context(year, evidence)) {
        year.clear();
    }

    QString effective_edition = edition;
    if (!edition.isEmpty() && pdf_compact(title).contains(pdf_compact(edition))) {
        effective_edition.clear();
    }
    return pdf_compose_name(title.simplified(), subtitle, authors, year, effective_edition, original);
}

const std::unordered_set<std::string> kDocumentExtensions = {
    ".txt", ".md", ".markdown", ".rtf", ".csv", ".tsv", ".log", ".json", ".xml", ".yml", ".yaml",
    ".ini", ".cfg", ".conf", ".html", ".htm", ".tex", ".rst", ".pdf", ".epub", ".docx", ".xlsx",
    ".pptx", ".odt", ".ods", ".odp"
};

const std::unordered_set<std::string> kTextExtensions = {
    ".txt", ".md", ".markdown", ".rtf", ".csv", ".tsv", ".log", ".json", ".xml", ".yml", ".yaml",
    ".ini", ".cfg", ".conf", ".html", ".htm", ".tex", ".rst"
};

const std::unordered_set<std::string> kStopwords = {
    "a", "an", "and", "are", "as", "at", "based", "be", "by", "category", "chapter",
    "description", "details", "document", "documents", "file", "files", "filename", "for",
    "from", "has", "in", "is", "it", "of", "on", "only", "page", "pages", "pdf", "doc",
    "docx", "rtf", "text", "this", "to", "txt", "with", "report", "notes", "note", "summary",
    "overview", "information", "data", "untitled", "ppt", "pptx", "xls", "xlsx", "odt", "ods",
    "odp", "presentation", "slides", "spreadsheet"
};

struct ParsedAnalysis {
    std::string summary;
    std::string filename;
    std::string type;
    std::string subject;
    std::string title;
};

std::optional<ParsedAnalysis> parse_analysis_json(const std::string& response) {
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;

    auto try_parse = [&](const std::string& payload) -> bool {
        std::istringstream stream(payload);
        return Json::parseFromStream(reader, stream, &root, &errors);
    };

    if (!try_parse(response)) {
        const auto first = response.find('{');
        const auto last = response.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first) {
            return std::nullopt;
        }
        if (!try_parse(response.substr(first, last - first + 1))) {
            return std::nullopt;
        }
    }

    if (!root.isObject()) {
        return std::nullopt;
    }

    ParsedAnalysis parsed;
    if (root.isMember("summary")) {
        parsed.summary = root["summary"].asString();
    } else if (root.isMember("description")) {
        parsed.summary = root["description"].asString();
    }

    if (root.isMember("type")) {
        parsed.type = root["type"].asString();
    } else if (root.isMember("document_type")) {
        parsed.type = root["document_type"].asString();
    }
    if (root.isMember("subject")) {
        parsed.subject = root["subject"].asString();
    }
    if (root.isMember("title")) {
        parsed.title = root["title"].asString();
    }

    if (root.isMember("filename")) {
        parsed.filename = root["filename"].asString();
    } else if (root.isMember("name")) {
        parsed.filename = root["name"].asString();
    } else if (root.isMember("suggested_name")) {
        parsed.filename = root["suggested_name"].asString();
    }

    return parsed;
}

std::string first_words(const std::string& text, size_t max_words) {
    std::istringstream iss(text);
    std::string word;
    std::string result;
    size_t count = 0;
    while (iss >> word) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += word;
        if (++count >= max_words) {
            break;
        }
    }
    return result;
}

} // namespace

DocumentTextAnalyzer::DocumentTextAnalyzer()
    : DocumentTextAnalyzer(Settings{})
{}

DocumentTextAnalyzer::DocumentTextAnalyzer(Settings settings)
    : settings_(settings) {
    if (settings_.max_characters == 0) {
        settings_.max_characters = kDefaultDocumentAnalyzerMaxCharacters;
    }
    if (settings_.max_tokens <= 0) {
        settings_.max_tokens = kDefaultDocumentAnalyzerMaxTokens;
    }
    if (settings_.max_filename_words == 0) {
        settings_.max_filename_words = kDefaultDocumentAnalyzerMaxFilenameWords;
    }
    if (settings_.max_filename_length == 0) {
        settings_.max_filename_length = kDefaultDocumentAnalyzerMaxFilenameLength;
    }
}

DocumentAnalysisResult DocumentTextAnalyzer::analyze(const std::filesystem::path& document_path,
                                                     ILLMClient& llm) const
{
    DocumentAnalysisResult result = analyze_generic(document_path, llm);
    if (!is_pdf(document_path) && !is_epub(document_path)) {
        return result;
    }

    try {
        const std::string strict_name = is_pdf(document_path)
                                            ? extract_pdf_name(document_path, llm)
                                            : extract_epub_name(document_path, llm);
        result.suggested_name = strict_name.empty()
                                    ? Utils::path_to_utf8(document_path.filename())
                                    : strict_name;
    } catch (...) {
        result.suggested_name = Utils::path_to_utf8(document_path.filename());
    }
    return result;
}

DocumentAnalysisResult DocumentTextAnalyzer::analyze_generic(
    const std::filesystem::path& document_path,
    ILLMClient& llm) const
{
    DocumentAnalysisResult result;
    const std::string raw_text = extract_text(document_path);
    if (raw_text.empty()) {
        throw std::runtime_error("No extractable text");
    }

    const std::string excerpt = truncate_excerpt(raw_text, settings_.max_characters);
    const auto metadata_title = extract_metadata_title(document_path);
    const std::string prompt = build_prompt(excerpt,
                                            Utils::path_to_utf8(document_path.filename()),
                                            metadata_title.value_or(""));
    const std::string response = llm.complete_prompt(prompt, settings_.max_tokens);

    std::string summary;
    std::string filename;
    std::string document_type;
    std::string subject;
    std::string title;
    if (auto parsed = parse_analysis_json(response)) {
        summary = trim(parsed->summary);
        filename = trim(parsed->filename);
        document_type = trim(parsed->type);
        subject = trim(parsed->subject);
        title = trim(parsed->title);
    }

    if (summary.empty()) {
        summary = trim(first_words(excerpt, 120));
    }

    document_type = FilenameTemplate::sanitize_component(document_type, settings_.ascii_fold, 2);
    std::string sanitized_subject = sanitize_filename(
        subject, settings_.max_filename_words, settings_.max_filename_length);
    if (sanitized_subject.empty()) {
        sanitized_subject = sanitize_filename(
            filename, settings_.max_filename_words, settings_.max_filename_length);
    }
    if (sanitized_subject.empty()) {
        sanitized_subject = sanitize_filename(
            summary, settings_.max_filename_words, settings_.max_filename_length);
    }
    if (sanitized_subject.empty()) {
        sanitized_subject = slugify(Utils::path_to_utf8(document_path.stem()));
    }
    if (sanitized_subject.empty()) {
        sanitized_subject = "document";
    }

    const std::string original_stem = Utils::path_to_utf8(document_path.stem());
    const std::string chosen_title = choose_document_title(
        metadata_title, original_stem, title, sanitized_subject);
    std::string sanitized_title = FilenameTemplate::sanitize_component(
        chosen_title, settings_.ascii_fold, 16, true);
    if (sanitized_title.empty()) {
        sanitized_title = sanitized_subject;
    }

    FilenameTemplate::Fields fields;
    if (const auto date = extract_creation_date(document_path)) {
        fields.date = *date;
        if (fields.date.size() >= 4) {
            fields.year = fields.date.substr(0, 4);
        }
    }
    fields.type = document_type;
    fields.title = sanitized_title;
    fields.subject = sanitized_subject;
    fields.original = original_stem;

    FilenameTemplate::Options template_options;
    template_options.ascii_fold = settings_.ascii_fold;
    template_options.max_length = settings_.template_max_length;
    std::string rendered = FilenameTemplate::render(
        settings_.filename_template, fields, template_options);
    if (rendered.empty()) {
        rendered = sanitized_subject;
    }

    result.summary = summary;
    result.document_type = document_type;
    result.subject = sanitized_subject;
    result.suggested_name = normalize_filename(rendered, document_path);
    return result;
}

std::string DocumentTextAnalyzer::extract_pdf_name(const std::filesystem::path& document_path,
                                                   ILLMClient& llm) const
{
    const auto pages = extract_pdf_pages(document_path, kPdfNameMaxPages);
    if (pages.empty()) {
        return {};
    }

    const auto metadata_title = extract_metadata_title(document_path);
    const std::string metadata_author = extract_pdf_author(document_path);
    const QString metadata_title_text =
        metadata_title ? pdf_qstring(*metadata_title) : QString();

    QString medical_evidence;
    for (const auto& page : pages) {
        medical_evidence += pdf_qstring(page.text);
        medical_evidence += QLatin1Char('\n');
    }
    if (const QString medical = pdf_medical_name(medical_evidence, document_path);
        !medical.isEmpty()) {
        return medical.toUtf8().toStdString();
    }

    const int budget = std::min(static_cast<int>(settings_.max_characters),
                                kPdfNameMaxEvidenceChars);
    const QString evidence = pdf_build_evidence(pages,
                                                metadata_title_text,
                                                pdf_qstring(metadata_author),
                                                budget);

    const std::string prompt = pdf_build_prompt().toUtf8().toStdString() +
                               "\n\nEVIDENCE:\n" + evidence.toUtf8().toStdString();
    const std::string response = llm.complete_prompt(
        prompt,
        std::max(settings_.max_tokens, kPdfNameMaxTokens));
    const auto parsed = parse_pdf_name_json(response);
    if (!parsed) {
        return {};
    }
    return pdf_choose_name(*parsed,
                           evidence,
                           metadata_title_text,
                           pdf_qstring(metadata_author),
                           pages,
                           document_path)
        .toUtf8()
        .toStdString();
}

std::string DocumentTextAnalyzer::extract_epub_name(const std::filesystem::path& document_path,
                                                    ILLMClient& llm) const
{
    const EpubMetadata metadata = extract_epub_metadata(document_path);
    const auto sections = extract_epub_sections(document_path, kPdfNameMaxPages);
    if (metadata.title.empty() && sections.empty()) {
        return {};
    }

    const int budget = std::min(static_cast<int>(settings_.max_characters),
                                kPdfNameMaxEvidenceChars);
    const QString evidence = epub_build_evidence(metadata, sections, budget);
    const std::string prompt = pdf_build_prompt().toUtf8().toStdString() +
                               "\n\nEVIDENCE:\n" + evidence.toUtf8().toStdString();
    const std::string response = llm.complete_prompt(
        prompt,
        std::max(settings_.max_tokens, kPdfNameMaxTokens));
    const auto parsed = parse_pdf_name_json(response);
    if (!parsed) {
        return {};
    }
    return pdf_choose_name(*parsed,
                           evidence,
                           pdf_qstring(metadata.title),
                           pdf_qstring(metadata.author),
                           sections,
                           document_path)
        .toUtf8()
        .toStdString();
}

bool DocumentTextAnalyzer::is_pdf(const std::filesystem::path& path) {
    if (!path.has_extension()) {
        return false;
    }
    return to_lower_copy(Utils::path_to_utf8(path.extension())) == ".pdf";
}

bool DocumentTextAnalyzer::is_epub(const std::filesystem::path& path) {
    if (!path.has_extension()) {
        return false;
    }
    return to_lower_copy(Utils::path_to_utf8(path.extension())) == ".epub";
}

bool DocumentTextAnalyzer::is_supported_document(const std::filesystem::path& path) {
    if (!path.has_extension()) {
        return false;
    }
    const std::string ext = to_lower_copy(Utils::path_to_utf8(path.extension()));
    return kDocumentExtensions.find(ext) != kDocumentExtensions.end();
}

std::optional<std::string> DocumentTextAnalyzer::extract_creation_date(const std::filesystem::path& path) {
    if (!path.has_extension()) {
        return std::nullopt;
    }
    const std::string ext = to_lower_copy(Utils::path_to_utf8(path.extension()));
    if (ext == ".pdf") {
        return extract_pdf_date(path);
    }
    if (ext == ".epub") {
        return extract_epub_date(path);
    }
    if (ext == ".docx") {
        return extract_docx_date(path);
    }
    return std::nullopt;
}

std::optional<std::string> DocumentTextAnalyzer::extract_metadata_title(const std::filesystem::path& path) {
    if (!path.has_extension()) {
        return std::nullopt;
    }
    const std::string ext = to_lower_copy(Utils::path_to_utf8(path.extension()));
    if (ext == ".pdf") {
#if defined(AI_FILE_SORTER_USE_PDFIUM)
        if (auto title = clean_metadata_title(extract_pdf_title_pdfium(path)); title.has_value()) {
            return title;
        }
#endif
        if (auto title = extract_pdf_title_via_pdfinfo(path); title.has_value()) {
            return title;
        }
        return std::nullopt;
    }
    if (ext == ".epub") {
        return extract_epub_title(path);
    }
    if (ext == ".docx") {
        return extract_docx_title(path);
    }
    if (ext == ".odt" || ext == ".ods" || ext == ".odp") {
        return extract_odf_title(path);
    }
    return std::nullopt;
}

std::string DocumentTextAnalyzer::prefer_descriptive_original_name(
    const std::string& original_name,
    const std::string& suggested_name)
{
    if (suggested_name.empty()) {
        return suggested_name;
    }

    const auto original_path = Utils::utf8_to_path(original_name);
    const std::string original_stem = Utils::path_to_utf8(original_path.stem());
    const std::string cleaned = clean_original_title(original_stem);
    if (!is_descriptive_original_title(cleaned)) {
        return suggested_name;
    }

    const auto suggested_path = Utils::utf8_to_path(suggested_name);
    const std::string suggested_stem = Utils::path_to_utf8(suggested_path.stem());
    if (count_title_words(suggested_stem) >= count_title_words(cleaned)) {
        return suggested_name;
    }

    std::string preferred = cleaned;
    for (char& ch : preferred) {
        switch (ch) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            ch = ' ';
            break;
        default:
            break;
        }
    }
    preferred = collapse_whitespace(preferred);
    while (!preferred.empty() &&
           (preferred.back() == '.' || preferred.back() == ' ')) {
        preferred.pop_back();
    }
    if (preferred.empty()) {
        return suggested_name;
    }
    return preferred + Utils::path_to_utf8(original_path.extension());
}

std::string DocumentTextAnalyzer::extract_text(const std::filesystem::path& path) const {
    if (!path.has_extension()) {
        return {};
    }
    const std::string ext = to_lower_copy(Utils::path_to_utf8(path.extension()));

    if (kTextExtensions.find(ext) != kTextExtensions.end()) {
        std::string text = read_file_prefix(path, settings_.max_characters);
        return collapse_whitespace(text);
    }

    if (ext == ".pdf") {
#if defined(AI_FILE_SORTER_USE_PDFIUM)
        if (auto output = extract_pdf_text_pdfium(path, settings_.max_characters); !output.empty()) {
            return collapse_whitespace(output);
        }
#endif
        const auto pdftotext = find_executable(QStringLiteral("pdftotext"));
        if (!pdftotext) {
            return {};
        }
        const QString file_path = path_to_qstring(path);
        auto output = run_process(*pdftotext,
                                  {QStringLiteral("-layout"), QStringLiteral("-q"), file_path, QStringLiteral("-")},
                                  15000);
        if (!output) {
            return {};
        }
        if (output->size() > settings_.max_characters) {
            output->resize(settings_.max_characters);
        }
        return collapse_whitespace(*output);
    }

    if (ext == ".epub") {
        return extract_epub_text(path, settings_.max_characters);
    }

    if (ext == ".docx") {
        auto xml = extract_zip_member(path, {QStringLiteral("word/document.xml")}, 7000);
        if (!xml) {
            return {};
        }
        std::string text = extract_xml_text(*xml);
        text = decode_basic_entities(text);
        if (text.size() > settings_.max_characters) {
            text.resize(settings_.max_characters);
        }
        return collapse_whitespace(text);
    }

    if (ext == ".xlsx") {
        auto xml = extract_zip_member(
            path,
            {QStringLiteral("xl/sharedStrings.xml"),
             QStringLiteral("xl/worksheets/sheet1.xml"),
             QStringLiteral("xl/worksheets/sheet2.xml")},
            7000);
        if (!xml) {
            return {};
        }
        std::string text = extract_xml_text(*xml);
        text = decode_basic_entities(text);
        if (text.size() > settings_.max_characters) {
            text.resize(settings_.max_characters);
        }
        return collapse_whitespace(text);
    }

    if (ext == ".pptx") {
        auto xml = extract_zip_member(
            path,
            {QStringLiteral("ppt/slides/slide1.xml"),
             QStringLiteral("ppt/slides/slide2.xml")},
            7000);
        if (!xml) {
            return {};
        }
        std::string text = extract_xml_text(*xml);
        text = decode_basic_entities(text);
        if (text.size() > settings_.max_characters) {
            text.resize(settings_.max_characters);
        }
        return collapse_whitespace(text);
    }

    if (ext == ".odt" || ext == ".ods" || ext == ".odp") {
        auto xml = extract_zip_member(path, {QStringLiteral("content.xml")}, 7000);
        if (!xml) {
            return {};
        }
        std::string text = extract_xml_text(*xml);
        text = decode_basic_entities(text);
        if (text.size() > settings_.max_characters) {
            text.resize(settings_.max_characters);
        }
        return collapse_whitespace(text);
    }

    return {};
}

std::string DocumentTextAnalyzer::build_prompt(const std::string& excerpt,
                                               const std::string& file_name,
                                               const std::string& metadata_title) const {
    std::ostringstream oss;
    oss << "Summarize the document excerpt below in at most 120 words. "
        << "Also classify it with a short lowercase type such as invoice, receipt, contract, report, letter, "
        << "manual, notes, spreadsheet, presentation, or form. "
        << "Then propose a short descriptive subject (max 3 words, nouns only). "
        << "Also propose a concise document title (up to 12 words) only when the excerpt clearly shows "
        << "the title or subtitle; otherwise return an empty title. Do not invent authors. "
        << "Use plain words, avoid generic words like 'document', 'file', or extensions. "
        << "Return JSON only in the format: "
        << "{\"summary\":\"...\",\"type\":\"...\",\"subject\":\"...\",\"title\":\"...\"}.\n\n";
    oss << "Document filename: " << file_name << "\n";
    if (!metadata_title.empty()) {
        oss << "Document metadata title: " << metadata_title << "\n";
        oss << "Prefer the metadata title when it is specific; otherwise derive the subject from the excerpt.\n";
    }
    oss << "Document excerpt:\n" << excerpt << "\n\n";
    oss << "JSON:";
    return oss.str();
}

std::string DocumentTextAnalyzer::sanitize_filename(const std::string& value,
                                                    size_t max_words,
                                                    size_t max_length) const {
    QString cleaned = sanitize_utf8_text(value).trimmed();
    const QString prefix = QStringLiteral("filename:");
    if (cleaned.startsWith(prefix, Qt::CaseInsensitive)) {
        cleaned = cleaned.mid(prefix.size()).trimmed();
    }
    const int newline = cleaned.indexOf('\n');
    if (newline != -1) {
        cleaned = cleaned.left(newline);
    }
    const int carriage = cleaned.indexOf('\r');
    if (carriage != -1) {
        cleaned = cleaned.left(carriage);
    }
    if (cleaned.size() >= 2) {
        const QChar first = cleaned.front();
        const QChar last = cleaned.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            cleaned = cleaned.mid(1, cleaned.size() - 2);
        }
    }

    auto words = split_words(cleaned);
    std::vector<std::string> filtered;
    filtered.reserve(words.size());
    std::unordered_set<std::string> seen;
    for (const auto& word : words) {
        if (word.empty()) {
            continue;
        }
        if (kStopwords.find(word) != kStopwords.end()) {
            continue;
        }
        if (seen.insert(word).second) {
            filtered.push_back(word);
        }
        if (filtered.size() >= max_words) {
            break;
        }
    }

    if (filtered.empty()) {
        return std::string();
    }

    QString joined;
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (i > 0) {
            joined.append(' ');
        }
        joined.append(QString::fromUtf8(filtered[i].c_str()));
    }

    if (joined.size() > static_cast<int>(max_length)) {
        joined = joined.left(static_cast<int>(max_length));
    }
    while (!joined.isEmpty() && (joined.endsWith('_') || joined.endsWith(' '))) {
        joined.chop(1);
    }

    return joined.toUtf8().toStdString();
}

std::string DocumentTextAnalyzer::trim(std::string value) {
    return trim_copy(value);
}

std::string DocumentTextAnalyzer::slugify(const std::string& value) {
    const QString input = sanitize_utf8_text(value);
    QString slug;
    slug.reserve(input.size());
    bool last_sep = false;
    for (const QChar ch : input) {
        if (ch.isLetterOrNumber()) {
            slug.append(ch.toLower());
            last_sep = false;
        } else if (!last_sep && !slug.isEmpty()) {
            slug.append(' ');
            last_sep = true;
        }
    }
    while (!slug.isEmpty() && (slug.endsWith('_') || slug.endsWith(' '))) {
        slug.chop(1);
    }
    if (slug.isEmpty()) {
        slug = QStringLiteral("document");
    }
    return slug.toUtf8().toStdString();
}

std::string DocumentTextAnalyzer::normalize_filename(const std::string& base,
                                                     const std::filesystem::path& original_path) {
    const std::string ext = Utils::path_to_utf8(original_path.extension());
    if (base.empty()) {
        return Utils::path_to_utf8(original_path.filename());
    }
    return ext.empty() ? base : base + ext;
}
