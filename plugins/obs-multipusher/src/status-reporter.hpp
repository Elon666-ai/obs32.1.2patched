#pragma once

#include "stream-ladder.hpp"
#include <string>
#include <mutex>

/// Reports pusher status to the ABR player backend via HTTP POST.
/// Callers drive the reporting cadence; the reporter is stateless
/// aside from the backend URL.
class StatusReporter {
public:
    StatusReporter() = default;
    ~StatusReporter() = default;

    /// Send a one-shot status update immediately.
    void SendNow(const StreamLadder& ladder, const std::string& statusOverride = "");

    /// Send final "Stopped" status before shutdown.
    void SendFinalStatus(const StreamLadder& ladder, const std::string& statusOverride = "Stopped");

    /// Set the backend URL for status reporting.
    void SetBackendURL(const std::string& url);

    /// Current backend URL.
    std::string BackendURL() const;

    /// Plugin version string for reporting.
    static constexpr const char* VERSION = "1.1.3";

private:
    void DoPost(const std::string& json);

    std::string backendURL_;
    mutable std::mutex mu_;
    std::string lastError_;
};
