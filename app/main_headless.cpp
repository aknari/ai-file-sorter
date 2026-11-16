/**
 * @file main_headless.cpp
 * @brief Entry point for the headless-only engine build.
 *
 * This binary links QtCore/QtNetwork only: no widgets, no GUI stack. It exposes
 * the same `--headless` command contract as the desktop application so existing
 * integrations (including File Organizer) can call it unchanged.
 */
#include "AppInfo.hpp"
#include "EmbeddedEnv.hpp"
#include "HeadlessAnalysisCommand.hpp"
#include "Settings.hpp"
#include "Utils.hpp"

#include <QCoreApplication>

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    HeadlessAnalysisCommand::ParseResult parsed = HeadlessAnalysisCommand::parse(argc, argv);

    // Keep Qt from seeing integration flags; the command parser owns them.
    char* qt_argv[] = { argc > 0 ? argv[0] : const_cast<char*>("FileOrganizerEngine"), nullptr };
    int qt_argc = 1;
    QCoreApplication app(qt_argc, qt_argv);

    // Match the desktop startup: Qt-derived paths and the embedded .env (model
    // download URLs/paths) must be in place before settings or the workflow load.
    QCoreApplication::setApplicationName(app_display_name());
    try {
        EmbeddedEnv env_loader(":/dev/hfstudio/AIFileSorter/.env");
        env_loader.load_env();
    } catch (const std::exception& error) {
        std::cerr << "warning: embedded environment unavailable: " << error.what() << "\n";
    }

    if (parsed.help_requested) {
        std::cout << HeadlessAnalysisCommand::usage_text();
        return HeadlessAnalysisCommand::ExitCode::Success;
    }

    if (!parsed.error.empty()) {
        std::cerr << parsed.error << "\n" << HeadlessAnalysisCommand::usage_text();
        return HeadlessAnalysisCommand::ExitCode::Usage;
    }

    if (!parsed.requested) {
        std::cerr << "error: this engine only runs headless operations; pass --headless.\n"
                  << HeadlessAnalysisCommand::usage_text();
        return HeadlessAnalysisCommand::ExitCode::Usage;
    }

    try {
        Settings settings;
        settings.load();
        const auto runtime_dir = Utils::utf8_to_path(settings.get_config_dir()) / "runtime";
        return HeadlessAnalysisCommand::run(parsed.options,
                                            runtime_dir,
                                            std::cout,
                                            std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return HeadlessAnalysisCommand::ExitCode::Failure;
    }
}
