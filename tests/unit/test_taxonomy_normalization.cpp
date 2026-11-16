#include <catch2/catch_test_macros.hpp>

#include "DatabaseManager.hpp"
#include "DatabaseTaxonomyNormalizer.hpp"
#include "TestHelpers.hpp"

#include <sqlite3.h>

#include <filesystem>

TEST_CASE("DatabaseTaxonomyNormalizer folds accents without dropping letters", "[taxonomy]") {
    using DatabaseTaxonomyNormalizer::normalize_label;

    CHECK(normalize_label("Películas") == "peliculas");
    CHECK(normalize_label("Pelculas") == "pelculas");
    CHECK(normalize_label("Guías") == "guias");
    CHECK(normalize_label("Guas") == "guas");
    CHECK(normalize_label("Música") == "musica");
    CHECK(normalize_label("Msica") == "msica");
    CHECK(normalize_label("Metodologías Ágiles") == "metodologias agiles");
    CHECK(normalize_label("Metodologas giles") == "metodologas giles");
    CHECK(normalize_label("日本語") == "日本語");
}

TEST_CASE("DatabaseManager keeps accented and truncated labels distinct", "[taxonomy]") {
    TempDir base_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", base_dir.path().string());
    DatabaseManager db(base_dir.path().string());

    const auto accented = db.resolve_category("Películas", "Metodologías Ágiles");
    const auto truncated = db.resolve_category("Pelculas", "Metodologas giles");

    REQUIRE(accented.taxonomy_id > 0);
    REQUIRE(truncated.taxonomy_id > 0);
    CHECK(accented.taxonomy_id != truncated.taxonomy_id);
    CHECK(accented.category == "Películas");
    CHECK(accented.subcategory == "Metodologías Ágiles");
    CHECK(truncated.category == "Pelculas");
    CHECK(truncated.subcategory == "Metodologas giles");

    const auto music = db.resolve_category("Música", "General");
    const auto broken_music = db.resolve_category("Msica", "General");
    REQUIRE(music.taxonomy_id > 0);
    REQUIRE(broken_music.taxonomy_id > 0);
    CHECK(music.taxonomy_id != broken_music.taxonomy_id);

    const auto guides = db.resolve_category("Guías", "General");
    const auto broken_guides = db.resolve_category("Guas", "General");
    REQUIRE(guides.taxonomy_id > 0);
    REQUIRE(broken_guides.taxonomy_id > 0);
    CHECK(guides.taxonomy_id != broken_guides.taxonomy_id);
}

TEST_CASE("DatabaseTaxonomyNormalizer accepts only minor label corrections", "[taxonomy]") {
    using DatabaseTaxonomyNormalizer::is_minor_label_correction;

    CHECK(is_minor_label_correction("Guas", "Guías"));
    CHECK(is_minor_label_correction("Ingeniera", "Ingeniería"));
    CHECK(is_minor_label_correction("Metodologa Scrum", "Metodología Scrum"));
    CHECK(is_minor_label_correction("Guías", "Guias"));
    CHECK(is_minor_label_correction("", ""));

    CHECK_FALSE(is_minor_label_correction("Guas", "Esternocleidomastoideo"));
    CHECK_FALSE(is_minor_label_correction("Guas", "Guas de usuario"));
    CHECK_FALSE(is_minor_label_correction("Guas", "Libros"));
    CHECK_FALSE(is_minor_label_correction("Guas", ""));
}

TEST_CASE("DatabaseManager applies user label overrides before cached translations", "[taxonomy]") {
    TempDir base_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", base_dir.path().string());
    DatabaseManager db(base_dir.path().string());

    const auto resolved = db.resolve_category("Guides", "General");
    REQUIRE(resolved.taxonomy_id > 0);
    REQUIRE(db.upsert_category_translation(resolved.taxonomy_id,
                                           CategoryLanguage::Spanish,
                                           "Guas",
                                           "General"));

    CHECK(db.localize_category(resolved, CategoryLanguage::Spanish).category == "Guas");

    db.set_user_label_override("Guides",
                               "General",
                               CategoryLanguage::Spanish,
                               "Guías",
                               "General");
    const auto overridden = db.localize_category(resolved, CategoryLanguage::Spanish);
    CHECK(overridden.category == "Guías");
    CHECK(overridden.subcategory == "General");

    CategorizedFile entry;
    entry.file_path = "/sample";
    entry.file_name = "guide.pdf";
    entry.type = FileType::File;
    entry.category = "Guides";
    entry.subcategory = "General";
    entry.taxonomy_id = resolved.taxonomy_id;
    entry.canonical_category = "Guides";
    entry.canonical_subcategory = "General";
    CHECK(db.localize_categorized_file(entry, CategoryLanguage::Spanish).category == "Guías");

    db.clear_user_label_overrides();
    CHECK(db.localize_category(resolved, CategoryLanguage::Spanish).category == "Guas");
}

TEST_CASE("DatabaseManager invalidates normalization caches on version change", "[taxonomy]") {
    TempDir base_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", base_dir.path().string());
    const std::string db_path = (base_dir.path() / "categorization_results.db").string();
    int translated_taxonomy_id = 0;

    {
        DatabaseManager db(base_dir.path().string());
        const auto resolved = db.resolve_category("Películas", "Metodologías Ágiles");
        REQUIRE(resolved.taxonomy_id > 0);
        translated_taxonomy_id = resolved.taxonomy_id;

        REQUIRE(db.upsert_category_translation(resolved.taxonomy_id,
                                               CategoryLanguage::French,
                                               "Películas",
                                               "Metodologías Ágiles"));
        REQUIRE(db.insert_or_update_file_with_categorization(
            "keep.mkv", "F", "/sample", resolved, false, std::string(), false, true));
        REQUIRE(db.insert_or_update_file_with_categorization(
            "drop.mkv", "F", "/sample", resolved, false, std::string(), false, false));
    }

    {
        sqlite3* raw_db = nullptr;
        REQUIRE(sqlite3_open(db_path.c_str(), &raw_db) == SQLITE_OK);
        char* error = nullptr;
        REQUIRE(sqlite3_exec(raw_db,
                             "UPDATE cache_metadata SET value = '1' "
                             "WHERE key = 'taxonomy_normalization_version';",
                             nullptr,
                             nullptr,
                             &error) == SQLITE_OK);
        if (error) {
            sqlite3_free(error);
        }
        REQUIRE(sqlite3_close(raw_db) == SQLITE_OK);
    }

    {
        DatabaseManager db(base_dir.path().string());

        const auto kept = db.get_categorized_file("/sample", "keep.mkv", FileType::File);
        REQUIRE(kept.has_value());
        CHECK(kept->rename_applied);
        CHECK(kept->taxonomy_id == 0);
        CHECK(kept->category == "Películas");

        const auto dropped = db.get_categorized_file("/sample", "drop.mkv", FileType::File);
        CHECK_FALSE(dropped.has_value());
        CHECK_FALSE(db.get_category_translation(translated_taxonomy_id,
                                                CategoryLanguage::French).has_value());
    }

    {
        sqlite3* raw_db = nullptr;
        REQUIRE(sqlite3_open(db_path.c_str(), &raw_db) == SQLITE_OK);
        const auto count = [&](const char* sql) {
            sqlite3_stmt* stmt = nullptr;
            REQUIRE(sqlite3_prepare_v2(raw_db, sql, -1, &stmt, nullptr) == SQLITE_OK);
            REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
            const int value = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            return value;
        };

        CHECK(count("SELECT COUNT(*) FROM category_taxonomy;") == 0);
        CHECK(count("SELECT COUNT(*) FROM category_translation;") == 0);
        CHECK(count("SELECT COUNT(*) FROM category_alias;") == 0);
        CHECK(count("SELECT COUNT(*) FROM file_categorization;") == 1);
        CHECK(count("SELECT CAST(value AS INTEGER) FROM cache_metadata "
                    "WHERE key = 'taxonomy_normalization_version';") == 2);
        sqlite3_close(raw_db);
    }
}
