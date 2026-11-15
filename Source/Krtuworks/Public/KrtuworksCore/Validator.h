#pragma once

#include <optional>
#include <string>
#include <vector>

#include "KrtuworksCore/Types.h"

namespace krtu
{
/// One of the validator's checks: PASSED, FAILED or SKIPPED.
struct ValidationCheck
{
    std::string key;
    std::string title;
    std::string status;
    /// Why it failed or was skipped.
    std::optional<std::string> detail;
};

struct ValidationReport
{
    /// No check failed.
    bool ok = false;
    std::string build;
    std::vector<ValidationCheck> checks;
};

struct ValidateOptions
{
    /// A web build signs in with the app's web client.
    bool web = false;
    /// The slot to write and read back; by default the app's first save slot in TEST.
    std::optional<std::string> saveSlot;
    /// A new TEST guest's date of birth (default 2000-01-01) and the terms it accepts (default the current ones).
    std::optional<std::string> birthDate;
    std::optional<std::string> acceptedTerms;
};

/// The Krtuworks validator, the rules of krtuworks-sdk-js/src/validator.ts: checks the
/// app's setup on this machine, then runs a TEST round against the servers the way the build's release report
/// counts it (a guest signs in, a session opens, sends a heartbeat and ends, an event batch, a save written and
/// read back, a progress operation, a test bug report, the remote config read). Every call names the build, so
/// this run turns its report green. `options` are the game's own (config, platform, build, stores, transport);
/// the run is always in TEST and uses those stores, so the same TEST guest comes back run after run. A run right
/// after another waits out the slot's write limit (one write per 10 seconds). Blocks; never throws.
ValidationReport validate(const Options& options, const ValidateOptions& settings = ValidateOptions());

/// What is wrong with krtuworks-config.json for this platform (a web build reads the web client), if anything.
std::vector<std::string> configProblems(const AppConfig& config, const std::string& platform, bool web);
/// What is wrong with the platform, if anything: a known one, and Steam with its Steamworks services.
std::vector<std::string> platformProblems(const std::string& platform, bool hasPlatformServices);
/// A send's verdict: sent, or left this device with no refusal (nothing); otherwise why it was not.
std::optional<std::string> sentOrWhy(int sent, int waiting, const std::optional<std::string>& stoppedBy);
}  // namespace krtu
