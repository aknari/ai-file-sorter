#pragma once

#include <cstddef>
#include <string>

/**
 * @brief Builds suggested filenames from a small, validated template language.
 *
 * Supported tokens: {date}, {time}, {year}, {type}, {subject}, {place},
 * {artist}, {album}, {title}, {original}, {hash8}.
 */
namespace FilenameTemplate {

inline constexpr const char* kDefaultDocumentTemplate = "{date} {type} {title}";
inline constexpr const char* kDefaultImageTemplate = "{date} {place} {subject}";

struct Fields {
    std::string date;
    std::string time;
    std::string year;
    std::string type;
    std::string subject;
    std::string place;
    std::string artist;
    std::string album;
    std::string title;
    std::string original;
    std::string hash8;
};

struct Options {
    bool ascii_fold{true};
    std::size_t max_length{100};
};

/**
 * @brief Validates a template before it is used.
 * @param text Template text.
 * @param error Optional error description.
 * @return True when every token is known and the template is filesystem-safe.
 */
bool validate(const std::string& text, std::string* error = nullptr);

/**
 * @brief Renders a template with the available fields.
 *
 * Empty fields disappear together with their separators. The result never
 * contains a path separator and never includes a file extension.
 */
std::string render(const std::string& text,
                   const Fields& fields,
                   const Options& options = {});

/**
 * @brief Sanitizes one semantic component (type, subject, place, ...).
 * @param value Raw text.
 * @param ascii_fold True to transliterate accented characters to ASCII.
 * @param max_words Maximum words to keep; 0 keeps all words.
 * @param preserve_case Keep the original letter casing when true.
 */
std::string sanitize_component(const std::string& value,
                               bool ascii_fold,
                               std::size_t max_words = 0,
                               bool preserve_case = false);

} // namespace FilenameTemplate
