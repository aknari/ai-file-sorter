#ifndef ANALYSIS_EXCLUSION_FILTER_HPP
#define ANALYSIS_EXCLUSION_FILTER_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @brief Rules used to skip files before analysis.
 *
 * All matching is case-insensitive. Extensions match the end of the file name,
 * name fragments match anywhere in the file name, and subfolder rules match
 * directories below the selected analysis folder.
 */
struct AnalysisExclusionRules {
    std::vector<std::string> extensions;
    std::vector<std::string> subfolders;
    std::vector<std::string> name_parts;

    bool empty() const
    {
        return extensions.empty() && subfolders.empty() && name_parts.empty();
    }

    std::size_t rule_count() const
    {
        return extensions.size() + subfolders.size() + name_parts.size();
    }

    /**
     * @brief Returns true when a path matches any configured exclusion rule.
     * @param root_path Selected analysis folder as UTF-8 text.
     * @param file_path Full path to test as UTF-8 text.
     */
    bool matches(const std::string& root_path, const std::string& file_path) const
    {
        if (empty() || file_path.empty()) {
            return false;
        }

        const std::filesystem::path path = exclusion_path_from_utf8(file_path);
        const std::string file_name = lower_ascii(exclusion_path_to_utf8(path.filename()));

        for (const auto& extension : extensions) {
            if (extension.empty()) {
                continue;
            }
            if (file_name.size() > extension.size() &&
                file_name.compare(file_name.size() - extension.size() - 1,
                                  extension.size() + 1,
                                  "." + extension) == 0) {
                return true;
            }
        }

        for (const auto& name_part : name_parts) {
            if (!name_part.empty() &&
                file_name.find(lower_ascii(name_part)) != std::string::npos) {
                return true;
            }
        }

        if (subfolders.empty() || root_path.empty()) {
            return false;
        }

        const std::filesystem::path root = exclusion_path_from_utf8(root_path).lexically_normal();
        const std::filesystem::path normalized_path = path.lexically_normal();
        const std::filesystem::path relative = normalized_path.lexically_relative(root);
        if (relative.empty()) {
            return false;
        }

        const std::filesystem::path relative_directory = relative.parent_path();
        if (relative_directory.empty()) {
            return false;
        }

        std::string directory = lower_ascii(
            normalize_separators(exclusion_path_to_utf8(relative_directory)));
        if (directory.empty() || directory == "." ||
            directory == ".." ||
            directory.rfind("../", 0) == 0) {
            return false;
        }

        for (const auto& rule : subfolders) {
            std::string pattern = normalize_separators(rule);
            pattern = lower_ascii(trim_copy(pattern));
            while (pattern.rfind("./", 0) == 0) {
                pattern.erase(0, 2);
            }
            while (!pattern.empty() && pattern.front() == '/') {
                pattern.erase(pattern.begin());
            }
            while (!pattern.empty() && pattern.back() == '/') {
                pattern.pop_back();
            }
            if (!pattern.empty() && relative_directory_matches(directory, pattern)) {
                return true;
            }
        }

        return false;
    }

private:
    static std::string trim_copy(const std::string& value)
    {
        const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
        auto trimmed = value;
        trimmed.erase(trimmed.begin(),
                      std::find_if(trimmed.begin(), trimmed.end(), not_space));
        trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), not_space).base(),
                      trimmed.end());
        return trimmed;
    }

    static std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    static std::string normalize_separators(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }

    static bool relative_directory_matches(const std::string& directory,
                                           const std::string& pattern)
    {
        if (pattern.find('/') != std::string::npos) {
            if (directory == pattern ||
                directory.rfind(pattern + "/", 0) == 0 ||
                directory.find("/" + pattern + "/") != std::string::npos) {
                return true;
            }
            const std::string suffix = "/" + pattern;
            return directory.size() >= suffix.size() &&
                   directory.compare(directory.size() - suffix.size(),
                                     suffix.size(),
                                     suffix) == 0;
        }

        std::size_t start = 0;
        while (start <= directory.size()) {
            const std::size_t end = directory.find('/', start);
            const std::size_t length = end == std::string::npos
                ? directory.size() - start
                : end - start;
            if (directory.compare(start, length, pattern) == 0 && length == pattern.size()) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    }

    static std::filesystem::path exclusion_path_from_utf8(const std::string& value)
    {
#ifdef _WIN32
        const auto* begin = reinterpret_cast<const char8_t*>(value.data());
        return std::filesystem::path(std::u8string(begin, begin + value.size()));
#else
        return std::filesystem::path(value);
#endif
    }

    static std::string exclusion_path_to_utf8(const std::filesystem::path& path)
    {
        const auto value = path.u8string();
        return std::string(value.begin(), value.end());
    }
};

/**
 * @brief Splits a comma/semicolon/newline separated exclusion input.
 */
inline std::vector<std::string> parse_exclusion_values(const std::string& value)
{
    std::vector<std::string> values;
    std::string current;
    const auto push_current = [&]() {
        const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
        auto trimmed = current;
        trimmed.erase(trimmed.begin(),
                      std::find_if(trimmed.begin(), trimmed.end(), not_space));
        trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), not_space).base(),
                      trimmed.end());
        if (!trimmed.empty() &&
            std::find(values.begin(), values.end(), trimmed) == values.end()) {
            values.push_back(std::move(trimmed));
        }
        current.clear();
    };

    for (const char ch : value) {
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r') {
            push_current();
        } else {
            current.push_back(ch);
        }
    }
    push_current();
    return values;
}

/**
 * @brief Parses extension rules, accepting values such as `tmp`, `.tmp` or `*.tmp`.
 */
inline std::vector<std::string> parse_exclusion_extensions(const std::string& value)
{
    std::vector<std::string> extensions;
    for (auto extension : parse_exclusion_values(value)) {
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension.rfind("*.", 0) == 0) {
            extension.erase(0, 2);
        } else if (extension.rfind("*", 0) == 0) {
            extension.erase(0, 1);
        }
        while (!extension.empty() && extension.front() == '.') {
            extension.erase(extension.begin());
        }
        if (!extension.empty() &&
            std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
            extensions.push_back(std::move(extension));
        }
    }
    return extensions;
}

/**
 * @brief Parses subfolder or filename fragment rules.
 */
inline std::vector<std::string> parse_exclusion_patterns(const std::string& value)
{
    return parse_exclusion_values(value);
}

#endif // ANALYSIS_EXCLUSION_FILTER_HPP
