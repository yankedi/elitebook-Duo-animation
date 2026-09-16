// ---------------------------------------------------------------------------
//  SensorLogger.h
//
//  Asynchronous CSV writer.
//
//  Sensor callbacks must never wait on disk I/O, so Push() only appends to a
//  small in-memory buffer under a short lock.  A dedicated writer thread drains
//  the buffer at most every 100 ms (or immediately when it gets large) and
//  flushes, which bounds both the I/O cost and the data lost on a crash.
// ---------------------------------------------------------------------------
#pragma once

#include "SensorTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dragonfly {

struct CsvRecord {
    int64_t wallUnixMs = 0;
    double steadySeconds = 0.0;

    bool hasAccel = false;
    Vec3 accel;

    bool hasGyro = false;
    Vec3 gyro;

    bool hasOrientation = false;
    Quat quat;

    bool hasInclination = false;
    double pitchDeg = 0.0;
    double rollDeg = 0.0;
    double yawDeg = 0.0;

    bool hasCompass = false;
    double headingDeg = 0.0;

    bool hasHinge = false;
    double hingeAngleDeg = 0.0;

    int lidMode = -1;            // -1 -> empty cell
    int simpleOrientation = -1;  // -1 -> empty cell

    // Fold estimator output (visual progress, not a physical angle).
    float foldProgress = 0.0f;
    float foldVelocity = 0.0f;
    float foldConfidence = 0.0f;
};

class SensorLogger {
public:
    SensorLogger();
    ~SensorLogger();

    SensorLogger(const SensorLogger&) = delete;
    SensorLogger& operator=(const SensorLogger&) = delete;

    // Creates <directory> if needed and opens sensor-YYYYMMDD-HHMMSS.csv.
    bool Open(const std::filesystem::path& directory);
    void Close();

    bool IsOpen() const { return m_open.load(std::memory_order_relaxed); }
    std::string Path() const;  // UTF-8

    void Push(const CsvRecord& record);

    uint64_t RowsWritten() const { return m_rows.load(std::memory_order_relaxed); }
    size_t RowsBuffered() const;

private:
    void WriterLoop();
    void WriteRow(const CsvRecord& record);

    std::atomic<bool> m_open{false};
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_rows{0};

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<CsvRecord> m_pending;

    std::thread m_writer;
    std::ofstream m_file;
    std::filesystem::path m_path;
};

} // namespace dragonfly
