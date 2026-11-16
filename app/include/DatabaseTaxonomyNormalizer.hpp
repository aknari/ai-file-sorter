#ifndef DATABASE_TAXONOMY_NORMALIZER_HPP
#define DATABASE_TAXONOMY_NORMALIZER_HPP

#include <string>

namespace DatabaseTaxonomyNormalizer {

/**
 * @brief Canonicalized category labels used for taxonomy lookup and storage.
 */
struct NormalizedCategory {
    std::string category;
    std::string subcategory;
    std::string normalized_category;
    std::string normalized_subcategory;
    std::string match_subcategory;
};

/**
 * @brief Normalize a category label for taxonomy comparisons.
 * @param input Display label to normalize.
 * @return Case-folded label with accents folded to base letters and normalized spacing.
 */
std::string normalize_label(const std::string& input);

/**
 * @brief Strip generic trailing file-type stopwords from an already normalized label.
 * @param normalized Normalized category or subcategory label.
 * @return Label without trailing generic stopwords when doing so preserves content.
 */
std::string strip_trailing_stopwords(const std::string& normalized);

/**
 * @brief Canonicalize category and subcategory labels for taxonomy resolution.
 * @param category User-facing main category label.
 * @param subcategory User-facing subcategory label.
 * @return Display and normalized labels, plus the subcategory form used for matching.
 */
NormalizedCategory normalize_category(std::string category, std::string subcategory);

/**
 * @brief Calculate normalized edit-distance similarity for taxonomy fuzzy matching.
 * @param a First normalized label.
 * @param b Second normalized label.
 * @return Similarity score in the range 0.0 to 1.0.
 */
double string_similarity(const std::string& a, const std::string& b);

/**
 * @brief Check whether a label edit is a small correction of the same label.
 *
 * Accent, case, punctuation, and whitespace-only changes always pass. Otherwise
 * the labels must keep the same word count and first letter, and stay within a
 * small edit distance. This rejects unrelated replacements such as
 * "Guas" -> "Esternocleidomastoideo" while allowing "Guas" -> "Guías".
 *
 * @param old_label Current display label.
 * @param new_label Proposed display label.
 * @param reason Optional output describing why the edit was rejected.
 * @return True when the edit is a minor correction of the same label.
 */
bool is_minor_label_correction(const std::string& old_label,
                               const std::string& new_label,
                               std::string* reason = nullptr);

} // namespace DatabaseTaxonomyNormalizer

#endif // DATABASE_TAXONOMY_NORMALIZER_HPP
