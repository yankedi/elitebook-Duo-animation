// ---------------------------------------------------------------------------
//  ConfigLoader.h
//
//  Reads src/config/config.json into a plain C++ structure.
//
//  Uses Windows.Data.Json (part of the OS) rather than pulling in a third
//  party parser, and keeps the WinRT dependency confined to this translation
//  unit so the estimator itself stays pure C++.
// ---------------------------------------------------------------------------
#pragma once

#include "../hinge/HingeTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace dragonfly {

struct AppConfig {
    HingeEstimatorConfig hinge;

    double animationSmoothingTauSeconds = 0.05;
    double animationSnapThresholdProgress = 0.25;

    bool debugWindowEnabled = true;
    int debugWindowWidth = 960;
    int debugWindowHeight = 640;

    // Path the values were read from; empty means built-in defaults were used.
    std::string loadedFrom;
};

// Tries each root in order ("<root>/config/config.json") and returns the first
// readable file.  A missing or malformed file is never fatal.
AppConfig LoadAppConfig(const std::vector<std::filesystem::path>& searchRoots);

// Writes the full set of values (handy as a template / for --write-config).
bool WriteConfigFile(const std::filesystem::path& path, const AppConfig& config);

// Renders the config as JSON text.
std::string ConfigToJson(const AppConfig& config);

} // namespace dragonfly
