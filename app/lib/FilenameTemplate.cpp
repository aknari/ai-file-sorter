#include "FilenameTemplate.hpp"

#include <QChar>
#include <QString>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FilenameTemplate {
namespace {

const std::unordered_set<std::string>& known_tokens()
{
    static const std::unordered_set<std::string> tokens = {
        "date", "time", "year", "type", "subject", "place",
        "artist", "album", "title", "original", "hash8"
    };
    return tokens;
}

std::string ascii_fold_utf8(const QString& input)
{
    const QString decomposed = input.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        const auto category = ch.category();
        if (category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing) {
            continue;
        }
        folded.append(ch);
    }
    return folded.normalized(QString::NormalizationForm_C).toUtf8().toStdString();
}

QString normalized_text(const std::string& value, bool ascii_fold)
{
    QString text = QString::fromUtf8(value.c_str());
    text.remove(QChar::ReplacementCharacter);
    text = text.normalized(QString::NormalizationForm_C);
    if (ascii_fold) {
        return QString::fromUtf8(ascii_fold_utf8(text).c_str());
    }
    return text;
}

void append_fragment(std::string& result, const std::string& fragment)
{
    if (fragment.empty()) {
        return;
    }
    if (!result.empty() && result.back() != ' ' && fragment.front() != ' ') {
        result.push_back(' ');
    }
    result.append(fragment);
}

std::string sanitize_literal(const std::string& value, bool ascii_fold)
{
    const QString text = normalized_text(value, ascii_fold).toLower();
    QString result;
    result.reserve(text.size());
    bool pending_space = false;
    for (const QChar ch : text) {
        if (ch.isLetterOrNumber()) {
            if (pending_space && !result.isEmpty()) {
                result.append(' ');
            }
            result.append(ch);
            pending_space = false;
        } else {
            pending_space = true;
        }
    }
    return result.toUtf8().toStdString();
}

std::string sanitize_date_like(const std::string& value, bool allow_colon)
{
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isdigit(ch)) {
            result.push_back(static_cast<char>(ch));
        } else if (ch == '-' || (allow_colon && ch == ':')) {
            result.push_back(static_cast<char>(ch));
        }
    }
    while (!result.empty() && (result.back() == '-' || result.back() == ':')) {
        result.pop_back();
    }
    return result;
}

std::string sanitize_token_value(const std::string& token,
                                 const std::string& value,
                                 const Options& options)
{
    if (token == "date") {
        return sanitize_date_like(value, false);
    }
    if (token == "time") {
        std::string compact;
        for (unsigned char ch : value) {
            if (std::isdigit(ch)) {
                compact.push_back(static_cast<char>(ch));
            }
        }
        return compact;
    }
    if (token == "year") {
        return sanitize_date_like(value, false);
    }
    if (token == "type") {
        return sanitize_component(value, options.ascii_fold, 2);
    }
    if (token == "subject") {
        return sanitize_component(value, options.ascii_fold, 3);
    }
    if (token == "place") {
        return sanitize_component(value, options.ascii_fold, 4);
    }
    if (token == "title") {
        return sanitize_component(value, options.ascii_fold, 16, true);
    }
    if (token == "original" || token == "artist" || token == "album" || token == "hash8") {
        return sanitize_component(value, options.ascii_fold, 0);
    }
    return std::string();
}

std::string field_value(const std::string& token, const Fields& fields)
{
    if (token == "date") return fields.date;
    if (token == "time") return fields.time;
    if (token == "year") return fields.year;
    if (token == "type") return fields.type;
    if (token == "subject") return fields.subject;
    if (token == "place") return fields.place;
    if (token == "artist") return fields.artist;
    if (token == "album") return fields.album;
    if (token == "title") return fields.title;
    if (token == "original") return fields.original;
    if (token == "hash8") return fields.hash8;
    return std::string();
}

std::string collapse_separators(std::string value)
{
    std::string collapsed;
    collapsed.reserve(value.size());
    bool previous_space = false;
    for (char ch : value) {
        if (ch == ' ' || ch == '_') {
            if (!previous_space && !collapsed.empty()) {
                collapsed.push_back(' ');
            }
            previous_space = true;
        } else {
            collapsed.push_back(ch);
            previous_space = false;
        }
    }
    while (!collapsed.empty() && (collapsed.back() == ' ' || collapsed.back() == '_')) {
        collapsed.pop_back();
    }
    while (!collapsed.empty() && (collapsed.front() == ' ' || collapsed.front() == '_')) {
        collapsed.erase(collapsed.begin());
    }
    return collapsed;
}

} // namespace

bool validate(const std::string& text, std::string* error)
{
    if (text.empty()) {
        if (error) *error = "Template is empty";
        return false;
    }

    std::size_t token_count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '}') {
            if (error) *error = "Unexpected closing brace";
            return false;
        }
        if (ch == '{') {
            const auto close = text.find('}', i + 1);
            if (close == std::string::npos) {
                if (error) *error = "Unclosed token";
                return false;
            }
            const std::string token = text.substr(i + 1, close - i - 1);
            if (known_tokens().find(token) == known_tokens().end()) {
                if (error) *error = "Unknown token: " + token;
                return false;
            }
            ++token_count;
            i = close;
            continue;
        }
        if (static_cast<unsigned char>(ch) < 0x20) {
            if (error) *error = "Template contains a control character";
            return false;
        }
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            if (error) *error = "Template contains a filesystem separator";
            return false;
        }
    }

    if (token_count == 0) {
        if (error) *error = "Template has no tokens";
        return false;
    }
    return true;
}

std::string sanitize_component(const std::string& value,
                               bool ascii_fold,
                               std::size_t max_words,
                               bool preserve_case)
{
    const QString text = normalized_text(value, ascii_fold);
    QString result;
    result.reserve(text.size());
    std::size_t words = 0;
    bool in_word = false;

    for (const QChar ch : text) {
        if (!ch.isLetterOrNumber()) {
            in_word = false;
            continue;
        }
        if (!in_word) {
            if (max_words > 0 && words >= max_words) {
                break;
            }
            if (!result.isEmpty()) {
                result.append(' ');
            }
            ++words;
            in_word = true;
        }
        result.append(preserve_case ? ch : ch.toLower());
    }

    return result.toUtf8().toStdString();
}

std::string render(const std::string& text,
                   const Fields& fields,
                   const Options& options)
{
    std::string result;
    result.reserve(text.size() + 64);
    std::string literal;

    auto flush_literal = [&]() {
        const std::string clean = sanitize_literal(literal, options.ascii_fold);
        literal.clear();
        append_fragment(result, clean);
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') {
            literal.push_back(text[i]);
            continue;
        }

        const auto close = text.find('}', i + 1);
        if (close == std::string::npos) {
            literal.append(text.substr(i));
            break;
        }

        flush_literal();
        const std::string token = text.substr(i + 1, close - i - 1);
        const std::string value = sanitize_token_value(
            token, field_value(token, fields), options);
        append_fragment(result, value);
        i = close;
    }
    flush_literal();

    result = collapse_separators(std::move(result));
    if (result.empty()) {
        return result;
    }

    const std::size_t max_length = options.max_length == 0 ? 100 : options.max_length;
    QString limited = QString::fromUtf8(result.c_str());
    if (static_cast<std::size_t>(limited.size()) > max_length) {
        limited = limited.left(static_cast<int>(max_length));
    }
    while (!limited.isEmpty() && (limited.back() == ' ' || limited.back() == '_')) {
        limited.chop(1);
    }
    return limited.toUtf8().toStdString();
}

} // namespace FilenameTemplate
