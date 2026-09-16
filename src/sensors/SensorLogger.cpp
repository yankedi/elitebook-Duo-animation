// ---------------------------------------------------------------------------
//  SensorLogger.cpp
// ---------------------------------------------------------------------------
#include "SensorLogger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace dragonfly {

namespace {

const char* kHeader =
    "timestamp,t_ms,"
    "accel_x,accel_y,accel_z,"
    "gyro_x,gyro_y,gyro_z,"
    "quat_x,quat_y,quat_z,quat_w,"
    "pitch,roll,yaw,"
    "compass,"
    "hinge_angle,"
    "lid_mode,"
    "simple_orientation,"
    "fold_progress,fold_velocity,fold_confidence";

std::string FormatLocalIso(int64_t unixMs) {
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
    const int millis = static_cast<int>(unixMs % 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  millis < 0 ? 0 : millis);
    return buffer;
}

std::string FormatTimestampForFileName() {
    const std::time_t seconds = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &seconds);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}

void AppendNumber(std::string& line, double value, int decimals) {
    if (!std::isfinite(value)) {
        return;  // empty cell
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    line += buffer;
}

} // namespace

SensorLogger::SensorLogger() = default;

SensorLogger::~SensorLogger() {
    Close();
}

bool SensorLogger::Open(const std::filesystem::path& directory) {
    if (m_open.load(std::memory_order_relaxed)) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    m_path = directory / ("sensor-" + FormatTimestampForFileName() + ".csv");
    m_file.open(m_path, std::ios::out | std::ios::trunc);
    if (!m_file.is_open()) {
        return false;
    }

    m_file << kHeader << '\n';
    m_file.flush();
    if (!m_file.good()) {
        return false;
    }

    m_running.store(true, std::memory_order_relaxed);
    m_open.store(true, std::memory_order_relaxed);
    m_rows.store(0, std::memory_order_relaxed);
    m_writer = std::thread(&SensorLogger::WriterLoop, this);
    return true;
}

void SensorLogger::Close() {
    if (!m_open.load(std::memory_order_relaxed)) {
        return;
    }
    m_open.store(false, std::memory_order_relaxed);
    m_running.store(false, std::memory_order_relaxed);
    m_cv.notify_all();
    if (m_writer.joinable()) {
        m_writer.join();
    }
    if (m_file.is_open()) {
        m_file.flush();
        m_file.close();
    }
}

std::string SensorLogger::Path() const {
    return m_path.string();
}

size_t SensorLogger::RowsBuffered() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

void SensorLogger::Push(const CsvRecord& record) {
    if (!m_open.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(record);
        // Keep the buffer bounded even if the disk stalls.
        if (m_pending.size() > 4096) {
            m_pending.erase(m_pending.begin(),
                            m_pending.begin() + static_cast<std::ptrdiff_t>(m_pending.size() - 4096));
        }
    }
    m_cv.notify_one();
}

void SensorLogger::WriterLoop() {
    std::vector<CsvRecord> batch;
    batch.reserve(512);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !m_running.load(std::memory_order_relaxed) || !m_pending.empty();
            });
            batch.swap(m_pending);
            if (batch.empty() && !m_running.load(std::memory_order_relaxed)) {
                break;
            }
        }

        for (const CsvRecord& record : batch) {
            WriteRow(record);
        }
        m_rows.fetch_add(batch.size(), std::memory_order_relaxed);
        batch.clear();

        if (m_file.is_open()) {
            m_file.flush();
        }
    }
}

void SensorLogger::WriteRow(const CsvRecord& record) {
    if (!m_file.is_open()) {
        return;
    }

    std::string line;
    line.reserve(256);

    line += FormatLocalIso(record.wallUnixMs);
    line += ',';
    AppendNumber(line, record.steadySeconds * 1000.0, 2);  // t_ms: monotonic
    line += ',';

    AppendNumber(line, record.hasAccel ? record.accel.x : std::nan(""), 5);
    line += ',';
    AppendNumber(line, record.hasAccel ? record.accel.y : std::nan(""), 5);
    line += ',';
    AppendNumber(line, record.hasAccel ? record.accel.z : std::nan(""), 5);
    line += ',';

    AppendNumber(line, record.hasGyro ? record.gyro.x : std::nan(""), 6);
    line += ',';
    AppendNumber(line, record.hasGyro ? record.gyro.y : std::nan(""), 6);
    line += ',';
    AppendNumber(line, record.hasGyro ? record.gyro.z : std::nan(""), 6);
    line += ',';

    AppendNumber(line, record.hasOrientation ? record.quat.x : std::nan(""), 6);
    line += ',';
    AppendNumber(line, record.hasOrientation ? record.quat.y : std::nan(""), 6);
    line += ',';
    AppendNumber(line, record.hasOrientation ? record.quat.z : std::nan(""), 6);
    line += ',';
    AppendNumber(line, record.hasOrientation ? record.quat.w : std::nan(""), 6);
    line += ',';

    AppendNumber(line, record.hasInclination ? record.pitchDeg : std::nan(""), 4);
    line += ',';
    AppendNumber(line, record.hasInclination ? record.rollDeg : std::nan(""), 4);
    line += ',';
    AppendNumber(line, record.hasInclination ? record.yawDeg : std::nan(""), 4);
    line += ',';

    AppendNumber(line, record.hasCompass ? record.headingDeg : std::nan(""), 4);
    line += ',';

    AppendNumber(line, record.hasHinge ? record.hingeAngleDeg : std::nan(""), 4);
    line += ',';

    if (record.lidMode >= 0) {
        line += std::to_string(record.lidMode);
    }
    line += ',';

    if (record.simpleOrientation >= 0) {
        line += std::to_string(record.simpleOrientation);
    }
    line += ',';

    AppendNumber(line, record.foldProgress, 4);
    line += ',';
    AppendNumber(line, record.foldVelocity, 4);
    line += ',';
    AppendNumber(line, record.foldConfidence, 3);

    line += '\n';
    m_file << line;
}

} // namespace dragonfly
