// ---------------------------------------------------------------------------
//  ConfigLoader.cpp
// ---------------------------------------------------------------------------
#include "ConfigLoader.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace dragonfly {

namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

double ReadNumber(const JsonObject& object, const wchar_t* key, double fallback) {
    if (!object || !object.HasKey(key)) {
        return fallback;
    }
    const auto value = object.Lookup(key);
    if (value.ValueType() == JsonValueType::Number) {
        return value.GetNumber();
    }
    return fallback;
}

int ReadInt(const JsonObject& object, const wchar_t* key, int fallback) {
    return static_cast<int>(ReadNumber(object, key, static_cast<double>(fallback)));
}

bool ReadBool(const JsonObject& object, const wchar_t* key, bool fallback) {
    if (!object || !object.HasKey(key)) {
        return fallback;
    }
    const auto value = object.Lookup(key);
    if (value.ValueType() == JsonValueType::Boolean) {
        return value.GetBoolean();
    }
    return fallback;
}

std::string ReadString(const JsonObject& object, const wchar_t* key,
                       const std::string& fallback) {
    if (!object || !object.HasKey(key)) {
        return fallback;
    }
    const auto value = object.Lookup(key);
    if (value.ValueType() == JsonValueType::String) {
        return winrt::to_string(value.GetString());
    }
    return fallback;
}

void ReadEstimatorConfig(const JsonObject& root, HingeEstimatorConfig& c) {
    if (!root || !root.HasKey(L"hingeEstimator")) {
        return;
    }
    const auto node = root.GetNamedObject(L"hingeEstimator");

    c.hingeAxis = ReadString(node, L"hingeAxis", c.hingeAxis);

    c.gyroDeadZoneDegPerSec = ReadNumber(node, L"gyroDeadZoneDegPerSec", c.gyroDeadZoneDegPerSec);
    c.gyroProgressGain = ReadNumber(node, L"gyroProgressGain", c.gyroProgressGain);
    c.rateSmoothingTauSeconds =
        ReadNumber(node, L"rateSmoothingTauSeconds", c.rateSmoothingTauSeconds);
    c.medianFilterLength = ReadInt(node, L"medianFilterLength", c.medianFilterLength);

    c.biasLearningRateLimitDegPerSec =
        ReadNumber(node, L"biasLearningRateLimitDegPerSec", c.biasLearningRateLimitDegPerSec);
    c.biasLearningDelaySeconds =
        ReadNumber(node, L"biasLearningDelaySeconds", c.biasLearningDelaySeconds);
    c.biasLearningTauSeconds =
        ReadNumber(node, L"biasLearningTauSeconds", c.biasLearningTauSeconds);

    c.motionStartThreshold = ReadNumber(node, L"motionStartThreshold", c.motionStartThreshold);
    c.stableThreshold = ReadNumber(node, L"stableThreshold", c.stableThreshold);
    c.settleTimeMs = ReadInt(node, L"settleTimeMs", c.settleTimeMs);

    c.state1MaxProgress = ReadNumber(node, L"state1MaxProgress", c.state1MaxProgress);
    c.state2MinProgress = ReadNumber(node, L"state2MinProgress", c.state2MinProgress);
    c.state2MaxProgress = ReadNumber(node, L"state2MaxProgress", c.state2MaxProgress);
    c.state3ForcesOpen = ReadBool(node, L"state3ForcesOpen", c.state3ForcesOpen);
    c.state1DefaultProgress = ReadNumber(node, L"state1DefaultProgress", c.state1DefaultProgress);
    c.state2DefaultProgress = ReadNumber(node, L"state2DefaultProgress", c.state2DefaultProgress);

    c.signProbeWindowSeconds = ReadNumber(node, L"signProbeWindowSeconds", c.signProbeWindowSeconds);
    c.signProbeMinRateDegPerSec =
        ReadNumber(node, L"signProbeMinRateDegPerSec", c.signProbeMinRateDegPerSec);

    c.idleRateFloorDegPerSec =
        ReadNumber(node, L"idleRateFloorDegPerSec", c.idleRateFloorDegPerSec);
    c.idleFreezeDelaySeconds =
        ReadNumber(node, L"idleFreezeDelaySeconds", c.idleFreezeDelaySeconds);
    c.idleFreezeTauSeconds =
        ReadNumber(node, L"idleFreezeTauSeconds", c.idleFreezeTauSeconds);

    c.orientationCorrectionGain =
        ReadNumber(node, L"orientationCorrectionGain", c.orientationCorrectionGain);
    c.accelDeviationLimitG = ReadNumber(node, L"accelDeviationLimitG", c.accelDeviationLimitG);
    c.axisShareFloor = ReadNumber(node, L"axisShareFloor", c.axisShareFloor);
    c.motionPenalty = ReadNumber(node, L"motionPenalty", c.motionPenalty);
    c.motionConfirmRateDegPerSec =
        ReadNumber(node, L"motionConfirmRateDegPerSec", c.motionConfirmRateDegPerSec);

    c.axisWindowSeconds = ReadNumber(node, L"axisWindowSeconds", c.axisWindowSeconds);
    c.axisSwitchRatio = ReadNumber(node, L"axisSwitchRatio", c.axisSwitchRatio);
    c.axisEnergyFloor = ReadNumber(node, L"axisEnergyFloor", c.axisEnergyFloor);
    c.axisEnergyMinRateDegPerSec =
        ReadNumber(node, L"axisEnergyMinRateDegPerSec", c.axisEnergyMinRateDegPerSec);

    c.manualStepPerPress = ReadNumber(node, L"manualStepPerPress", c.manualStepPerPress);
}

void ReadAppSections(const JsonObject& root, AppConfig& config) {
    ReadEstimatorConfig(root, config.hinge);

    if (root && root.HasKey(L"animation")) {
        const auto node = root.GetNamedObject(L"animation");
        config.animationSmoothingTauSeconds = ReadNumber(
            node, L"smoothingTauSeconds", config.animationSmoothingTauSeconds);
        config.animationSnapThresholdProgress =
            ReadNumber(node, L"snapThresholdProgress", config.animationSnapThresholdProgress);
    }

    if (root && root.HasKey(L"debugWindow")) {
        const auto node = root.GetNamedObject(L"debugWindow");
        config.debugWindowEnabled = ReadBool(node, L"enabled", config.debugWindowEnabled);
        config.debugWindowWidth = ReadInt(node, L"width", config.debugWindowWidth);
        config.debugWindowHeight = ReadInt(node, L"height", config.debugWindowHeight);
    }
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

AppConfig LoadAppConfig(const std::vector<std::filesystem::path>& searchRoots) {
    AppConfig config;

    for (const std::filesystem::path& root : searchRoots) {
        const std::filesystem::path path = root / "config" / "config.json";
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            continue;
        }
        const std::string text = ReadFileText(path);
        if (text.empty()) {
            continue;
        }
        try {
            const auto root_object = JsonObject::Parse(winrt::to_hstring(text));
            ReadAppSections(root_object, config);
            config.loadedFrom = path.string();
            return config;
        } catch (const winrt::hresult_error&) {
            // Malformed JSON: fall through to the next candidate / defaults.
        }
    }

    return config;
}

std::string ConfigToJson(const AppConfig& config) {
    const HingeEstimatorConfig& c = config.hinge;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(4);

    out << "{\n";
    out << "  \"hingeEstimator\": {\n";
    out << "    \"hingeAxis\": \"" << c.hingeAxis << "\",\n";
    out << "    \"gyroDeadZoneDegPerSec\": " << c.gyroDeadZoneDegPerSec << ",\n";
    out << "    \"gyroProgressGain\": " << c.gyroProgressGain << ",\n";
    out << "    \"rateSmoothingTauSeconds\": " << c.rateSmoothingTauSeconds << ",\n";
    out << "    \"medianFilterLength\": " << c.medianFilterLength << ",\n";
    out << "    \"biasLearningRateLimitDegPerSec\": " << c.biasLearningRateLimitDegPerSec << ",\n";
    out << "    \"biasLearningDelaySeconds\": " << c.biasLearningDelaySeconds << ",\n";
    out << "    \"biasLearningTauSeconds\": " << c.biasLearningTauSeconds << ",\n";
    out << "    \"motionStartThreshold\": " << c.motionStartThreshold << ",\n";
    out << "    \"stableThreshold\": " << c.stableThreshold << ",\n";
    out << "    \"settleTimeMs\": " << c.settleTimeMs << ",\n";
    out << "    \"state1MaxProgress\": " << c.state1MaxProgress << ",\n";
    out << "    \"state2MinProgress\": " << c.state2MinProgress << ",\n";
    out << "    \"state2MaxProgress\": " << c.state2MaxProgress << ",\n";
    out << "    \"state3ForcesOpen\": " << (c.state3ForcesOpen ? "true" : "false") << ",\n";
    out << "    \"state1DefaultProgress\": " << c.state1DefaultProgress << ",\n";
    out << "    \"state2DefaultProgress\": " << c.state2DefaultProgress << ",\n";
    out << "    \"signProbeWindowSeconds\": " << c.signProbeWindowSeconds << ",\n";
    out << "    \"signProbeMinRateDegPerSec\": " << c.signProbeMinRateDegPerSec << ",\n";
    out << "    \"idleRateFloorDegPerSec\": " << c.idleRateFloorDegPerSec << ",\n";
    out << "    \"idleFreezeDelaySeconds\": " << c.idleFreezeDelaySeconds << ",\n";
    out << "    \"idleFreezeTauSeconds\": " << c.idleFreezeTauSeconds << ",\n";
    out << "    \"orientationCorrectionGain\": " << c.orientationCorrectionGain << ",\n";
    out << "    \"accelDeviationLimitG\": " << c.accelDeviationLimitG << ",\n";
    out << "    \"axisShareFloor\": " << c.axisShareFloor << ",\n";
    out << "    \"motionPenalty\": " << c.motionPenalty << ",\n";
    out << "    \"motionConfirmRateDegPerSec\": " << c.motionConfirmRateDegPerSec << ",\n";
    out << "    \"axisWindowSeconds\": " << c.axisWindowSeconds << ",\n";
    out << "    \"axisSwitchRatio\": " << c.axisSwitchRatio << ",\n";
    out << "    \"axisEnergyFloor\": " << c.axisEnergyFloor << ",\n";
    out << "    \"axisEnergyMinRateDegPerSec\": " << c.axisEnergyMinRateDegPerSec << ",\n";
    out << "    \"manualStepPerPress\": " << c.manualStepPerPress << "\n";
    out << "  },\n";
    out << "  \"animation\": {\n";
    out << "    \"smoothingTauSeconds\": " << config.animationSmoothingTauSeconds << ",\n";
    out << "    \"snapThresholdProgress\": " << config.animationSnapThresholdProgress << "\n";
    out << "  },\n";
    out << "  \"debugWindow\": {\n";
    out << "    \"enabled\": " << (config.debugWindowEnabled ? "true" : "false") << ",\n";
    out << "    \"width\": " << config.debugWindowWidth << ",\n";
    out << "    \"height\": " << config.debugWindowHeight << "\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

bool WriteConfigFile(const std::filesystem::path& path, const AppConfig& config) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << ConfigToJson(config);
    return file.good();
}

} // namespace dragonfly
