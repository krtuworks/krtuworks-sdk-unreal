#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Api.h"
#include "Events.h"

namespace krtu::detail
{
/// Bug reports from inside the app, the rules of krtuworks-sdk-js/src/bugs.ts: a report is
/// kept on the device first and sent when it can be (offline, before the player exists, after a merge), its
/// screenshot right after it. The body is frozen when the report is made, so sending it again after a lost
/// answer gets the same report. The last events and log lines are kept in memory only.
class BugReports
{
public:
    BugReports(const Resolved& opts, KeyValueStore& store, Api& api, std::function<std::string()> owner, std::function<std::optional<std::string>()> subject,
               std::function<bool(const BugReportView&)> fixedTold);

    /// The fixed categories: the same menu in every app.
    static const std::vector<std::string>& categories();
    /// The picture's type from its first bytes (what the server takes); nothing for anything else.
    static std::optional<std::string> screenshotType(std::string_view bytes);
    /// The body the server takes for a report: the player's words and what the SDK knows of the moment. Checked
    /// here (std::invalid_argument) so a report that can never be taken is refused at once.
    static Json reportBody(const BugReportInput& input, const std::string& reportId, const Resolved& opts, const std::optional<std::string>& level,
                           const std::optional<double>& fps, const std::vector<Json>& events, const std::string& log);

    struct Fixed
    {
        std::vector<BugReportView> fixed;
        std::vector<std::string> told;
    };
    /// The reports to tell about since the last check: the first check of a player on this device (`told`
    /// unknown) tells nothing; a report made here starts the record empty.
    static Fixed newlyFixed(const std::vector<BugReportView>& reports, const std::optional<std::vector<std::string>>& told);

    /// An event the SDK kept, for the next report's "last events".
    void noteEvent(const WireEvent& event);
    /// A line for the next report's log (the app's own, and the errors the SDK told).
    void log(const std::string& message);
    /// Keeps a report on this device (for the player, or for the first player); returns its id.
    std::string report(const BugReportInput& input, const std::optional<std::string>& level);
    /// Reports (with their screenshots) that wait on this device for the player, or for the first player.
    int waiting();
    /// Hands one owner's waiting reports and history to another (the anonymous queue to the first player, a confirmed merge).
    void move(const std::string& from, const std::string& to);
    BugFlushResult flush();
    /// The player's reports in this app and environment, newest first; fixes not told yet are told.
    std::vector<BugReportView> mine();
    /// At a player's arrival: only when this device keeps a record for them (they reported here, or the app read their reports).
    void checkFixed();
    void resetBackoff();

private:
    std::string prefix(const std::string& who) const;
    std::string fixedKey(const std::string& who) const;
    void tellFixed(const std::string& who, const std::vector<BugReportView>& reports);

    const Resolved& opts_;
    KeyValueStore& store_;
    Api& api_;
    std::function<std::string()> owner_;
    std::function<std::optional<std::string>()> subject_;
    /// Tells the game a fix; false when no one listens (the fix then waits for the next look).
    std::function<bool(const BugReportView&)> fixedTold_;
    std::mutex mutex_;
    /// The history is kept with the owner it was made under; a report takes only its own owner's.
    struct NotedEvent
    {
        std::string owner;
        Json entry;
    };
    struct NotedLine
    {
        std::string owner;
        std::string line;
    };
    struct NotedFps
    {
        std::string owner;
        double fps = 0;
    };
    std::vector<NotedEvent> events_;
    std::vector<NotedLine> logLines_;
    std::size_t logBytes_ = 0;
    std::optional<NotedFps> lastFps_;
    /// Reports that wait before their next try (in memory: a new launch tries them all again).
    std::map<std::string, std::int64_t> waitUntil_;
};
}  // namespace krtu::detail
