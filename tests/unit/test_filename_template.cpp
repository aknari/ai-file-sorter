#include <catch2/catch_test_macros.hpp>

#include "FilenameTemplate.hpp"

#include <string>

TEST_CASE("Filename template renders document fields", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.date = "2024-05-12";
    fields.type = "Invoice";
    fields.subject = "Acme January";
    fields.title = "Acme January";
    fields.original = "scan001";

    const std::string rendered = FilenameTemplate::render(
        FilenameTemplate::kDefaultDocumentTemplate, fields);
    CHECK(rendered == "2024-05-12 invoice Acme January");
}

TEST_CASE("Filename template drops empty fields and separators", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.type = "";
    fields.subject = "Holiday Photos";

    CHECK(FilenameTemplate::render("{date}_{type}_{subject}", fields) == "holiday photos");
    CHECK(FilenameTemplate::render("{date}-{subject}", fields) == "holiday photos");
}

TEST_CASE("Filename template folds accents and enforces word limits", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.subject = "Facturación año pasado";

    CHECK(FilenameTemplate::render("{subject}", fields) == "facturacion ano pasado");

    FilenameTemplate::Options options;
    options.ascii_fold = false;
    CHECK(FilenameTemplate::render("{subject}", fields, options) == "facturación año pasado");
}

TEST_CASE("Filename template renders image fields", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.date = "2014-03-10";
    fields.place = "Venice";
    fields.subject = "Black Ducks";

    CHECK(FilenameTemplate::render(FilenameTemplate::kDefaultImageTemplate, fields) ==
          "2014-03-10 venice black ducks");
}

TEST_CASE("Filename template preserves metadata title casing", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.title = "Quarterly Report";

    CHECK(FilenameTemplate::render("{title}", fields) == "Quarterly Report");
}

TEST_CASE("Filename template keeps long document titles", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.title = "one two three four five six seven eight nine ten eleven twelve";

    CHECK(FilenameTemplate::render("{title}", fields) ==
          "one two three four five six seven eight nine ten eleven twelve");
}

TEST_CASE("Filename template validates unknown tokens and separators", "[filename-template]")
{
    std::string error;
    CHECK(FilenameTemplate::validate("{date}_{type}_{subject}", &error));
    CHECK_FALSE(FilenameTemplate::validate("{date}_{unknown}", &error));
    CHECK_FALSE(FilenameTemplate::validate("{date}/{subject}", &error));
    CHECK_FALSE(FilenameTemplate::validate("", &error));
}

TEST_CASE("Filename template truncates to the requested length", "[filename-template]")
{
    FilenameTemplate::Fields fields;
    fields.subject = "alpha beta gamma";

    FilenameTemplate::Options options;
    options.max_length = 8;
    CHECK(FilenameTemplate::render("{subject}", fields, options) == "alpha be");
}
