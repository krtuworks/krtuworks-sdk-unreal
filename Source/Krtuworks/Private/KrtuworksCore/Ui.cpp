#include "KrtuworksCore/Ui.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <stdexcept>
#include <thread>

#include "Infrastructure.h"
#include "KrtuworksCore/Qr.h"

namespace krtu
{
namespace
{
const char* const underageKey = "krtu:ui:underageUntil";

UiResult signedIn(const Player& player)
{
    UiResult result;
    result.status = UiStatus::SignedIn;
    result.player = player;
    return result;
}

UiResult offline(UiOfflineReason reason)
{
    UiResult result;
    result.status = UiStatus::Offline;
    result.reason = reason;
    return result;
}

UiResult cancelled()
{
    UiResult result;
    result.status = UiStatus::Cancelled;
    return result;
}

/// What the two threads of a sign-in tell the one that decides: the address that was opened, the player who
/// signed in, a failure, or the player's answer to the screen.
struct Event
{
    enum class Kind
    {
        Opened,
        Signed,
        Failed,
        Answered
    };
    Kind kind = Kind::Answered;
    int generation = 0;
    std::string url;
    std::optional<Player> player;
    std::exception_ptr error;
    UiAnswer answer;
};

class Mailbox
{
public:
    void post(Event event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(event));
        }
        wake_.notify_one();
    }

    /// The next event; nothing when none came within `timeout`.
    std::optional<Event> next(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, timeout, [this] { return !queue_.empty(); });
        if (queue_.empty()) return std::nullopt;
        Event event = std::move(queue_.front());
        queue_.pop_front();
        return event;
    }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Event> queue_;
};

/// The thread that waits for the player's answer to the screen of a sign-in. Stopping it dismisses the screen and closes it.
class Shower
{
public:
    explicit Shower(UiHost& host) : host_(host) {}
    Shower(const Shower&) = delete;
    Shower& operator=(const Shower&) = delete;
    ~Shower() { stop(); }

    /// Runs `work` on a thread of its own once the previous one has returned.
    void start(const std::function<void(const CancelToken&)>& work)
    {
        join();
        dismiss_ = CancelToken();
        const CancelToken dismiss = dismiss_;
        thread_ = std::thread([work, dismiss] { work(dismiss); });
    }

    void stop()
    {
        dismiss_.cancel();
        join();
        host_.close();
    }

private:
    void join()
    {
        if (thread_.joinable()) thread_.join();
    }

    UiHost& host_;
    CancelToken dismiss_;
    std::thread thread_;
};

/// A thread whose wait a token ends; leaving the scope ends it and waits for it.
class Waiter
{
public:
    Waiter() = default;
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    ~Waiter() { stop(); }

    void start(const CancelToken& token, const std::function<void()>& work)
    {
        stop();
        token_ = token;
        thread_ = std::thread(work);
    }

    void stop()
    {
        token_.cancel();
        if (thread_.joinable()) thread_.join();
    }

private:
    CancelToken token_;
    std::thread thread_;
};

std::string remembered(const std::optional<std::string>& stored)
{
    if (!stored) return "";
    const auto json = Json::parse(*stored);
    return json && json->isString() ? json->asString() : *stored;
}
}  // namespace

struct UiController::AgeOutcome
{
    std::string birthDate;
    std::optional<UiResult> result;
};

ErrorScreen errorScreen(const Error& err, const UiTexts& texts, const std::string& locale, int utcOffsetMinutes)
{
    const std::string& code = err.code();
    if (code == "network_error") return {textFor(texts, "error.offline"), true};
    if (code == "sanctioned")
    {
        const std::string reason = err.details().getString("reason").value_or("");
        std::string message = reason.empty() ? textFor(texts, "error.banned") : textFor(texts, "error.bannedReason", {{"reason", reason}});
        const auto endsAt = err.details().getString("endsAt");
        const auto endsAtMs = endsAt ? detail::parseTimeMs(*endsAt) : std::nullopt;
        if (endsAtMs) message += " " + textFor(texts, "error.bannedUntil", {{"date", longDate(*endsAtMs, utcOffsetMinutes, locale, texts)}});
        return {message, false};
    }
    if (code == "guest_limit" || code == "guest_capacity") return {textFor(texts, "error.busy"), true};
    if (code == "app_suspended" || code == "maintenance") return {textFor(texts, "error.maintenance"), true};
    if (code == "access_denied" || code == "cancelled") return {textFor(texts, "error.cancelled"), true};
    return {textFor(texts, err.status() == 0 ? "error.offline" : "error.generic"), true};
}

ErrorScreen errorScreen(const UiTexts& texts)
{
    return {textFor(texts, "error.generic"), true};
}

UiController::UiController(std::shared_ptr<Client> client, std::shared_ptr<UiHost> host, UiOptions options)
    : client_(std::move(client)), host_(std::move(host)), options_(std::move(options))
{
    if (!client_ || !host_) throw std::invalid_argument("the ready-made screens need a client and a host");
    if (options_.signInMode == UiSignInMode::Browser && !options_.openUrl) throw std::invalid_argument("the browser sign-in needs UiOptions::openUrl");
    if (options_.signInMode == UiSignInMode::App && !options_.authenticate) throw std::invalid_argument("the phone sign-in needs UiOptions::authenticate");
    if (!options_.memory) options_.memory = std::make_shared<MemoryStore>();
    if (!options_.now) options_.now = [] { return detail::nowMs(); };
    if (!options_.utcOffsetMinutes) options_.utcOffsetMinutes = systemUtcOffsetMinutes;
}

std::int64_t UiController::nowMs() const
{
    return options_.now();
}

int UiController::offsetAt(std::int64_t unixMs) const
{
    return options_.utcOffsetMinutes(unixMs);
}

std::string UiController::t(const std::string& key, const std::map<std::string, std::string>& vars) const
{
    return textFor(options_.texts, key, vars);
}

std::string UiController::platformName(const std::string& platform) const
{
    const std::string key = "platform." + platform;
    return uiDefaultTexts().count(key) > 0 || options_.texts.count(key) > 0 ? t(key) : platform;
}

bool UiController::signInOffered() const
{
    return options_.signInMode != UiSignInMode::None;
}

UiAnswer UiController::once(const UiScreen& screen)
{
    const UiAnswer answer = host_->show(screen, CancelToken());
    host_->close();
    if (answer.kind == UiAnswerKind::Closed) throw HostClosed{};
    return answer;
}

// ── Who plays ───────────────────────────────────────────────────────────

UiResult UiController::ensurePlayer()
{
    std::lock_guard<std::recursive_mutex> lock(flow_);
    try
    {
        if (const auto player = client_->player()) return signedIn(*player);
        if (const auto blocked = gateProblem()) return failOffline(*blocked);
        if (underageNow()) return offline(UiOfflineReason::Underage);
        if (!options_.guests)
        {
            const UiResult result = signIn();
            return result.status == UiStatus::Cancelled ? offline(UiOfflineReason::Unavailable) : result;
        }
        return guestFlow();
    }
    catch (const HostClosed&)
    {
        return offline(UiOfflineReason::Unavailable);
    }
}

UiResult UiController::guestFlow()
{
    std::optional<std::string> birthDate;
    std::optional<std::string> terms;
    for (;;)
    {
        std::optional<Error> failure;
        try
        {
            return signedIn(client_->signInAsGuest(birthDate, terms));
        }
        catch (const Error& err)
        {
            failure = err;
        }
        catch (const std::exception&)
        {
            // Not the SDK's own failure: the generic error screen below.
        }
        if (failure)
        {
            const std::string& code = failure->code();
            if (code == "birth_date_required" || code == "invalid_birth_date")
            {
                const AgeOutcome outcome = askAge(code == "invalid_birth_date" ? AgeProblem::Invalid : AgeProblem::None);
                if (outcome.result) return *outcome.result;
                birthDate = outcome.birthDate;
                terms = std::string(Client::currentTerms);
                continue;
            }
            if (code == "too_young")
            {
                if (birthDate) rememberUnderage(*birthDate);
                UiScreen screen;
                screen.kind = UiScreenKind::Underage;
                once(screen);
                return offline(UiOfflineReason::Underage);
            }
            const auto asked = failure->details().getString("terms");
            if (code == "terms_required" && asked && asked != terms)
            {
                UiScreen screen;
                screen.kind = UiScreenKind::Terms;
                screen.termsUrl = uiTermsUrl;
                screen.privacyUrl = uiPrivacyUrl;
                once(screen);
                terms = asked;
                continue;
            }
        }
        const ErrorScreen shown = failure ? errorScreen(*failure, options_.texts, options_.locale, offsetAt(nowMs())) : errorScreen(options_.texts);
        UiScreen screen;
        screen.kind = UiScreenKind::Error;
        screen.message = shown.message;
        screen.retry = shown.retry;
        if (once(screen).kind != UiAnswerKind::Retry) return offline(UiOfflineReason::Unavailable);
    }
}

UiResult UiController::failOffline(const ErrorScreen& shown)
{
    UiScreen screen;
    screen.kind = UiScreenKind::Error;
    screen.message = shown.message;
    screen.retry = false;
    try
    {
        once(screen);
    }
    catch (const HostClosed&)
    {
        // Nobody could be shown the message: the game keeps going offline all the same.
    }
    return offline(UiOfflineReason::Unavailable);
}

std::optional<ErrorScreen> UiController::gateProblem() const
{
    const Gate gate = client_->getGate();
    if (gate.maintenance)
    {
        const bool own = gate.maintenanceMessage && !gate.maintenanceMessage->empty();
        return ErrorScreen{own ? *gate.maintenanceMessage : t("error.maintenance"), false};
    }
    if (gate.updateRequired) return ErrorScreen{t("error.update"), false};
    return std::nullopt;
}

/// The age screen until it has a real answer: a birth date, the sign-in, or "under 13" (remembered).
UiController::AgeOutcome UiController::askAge(AgeProblem first)
{
    AgeProblem problem = first;
    for (;;)
    {
        const std::int64_t now = nowMs();
        UiScreen screen;
        screen.kind = UiScreenKind::Age;
        screen.order = dateOrder(options_.locale);
        screen.years = birthYears(now);
        screen.termsUrl = uiTermsUrl;
        screen.privacyUrl = uiPrivacyUrl;
        screen.signIn = signInOffered();
        screen.problem = problem;
        const UiAnswer answer = once(screen);
        if (answer.kind == UiAnswerKind::SignIn)
        {
            const UiResult result = signIn();
            if (result.status != UiStatus::Cancelled) return {"", result};
            // The player came back without signing in.
            problem = AgeProblem::None;
            continue;
        }
        if (answer.kind != UiAnswerKind::Birth) continue;
        const BirthCheck verdict = checkBirth(answer.year, answer.month, answer.day, now);
        if (verdict == BirthCheck::Invalid)
        {
            problem = AgeProblem::Invalid;
            continue;
        }
        const std::string birthDate = isoDate(answer.year, answer.month, answer.day);
        if (verdict == BirthCheck::Underage)
        {
            rememberUnderage(birthDate);
            UiScreen under;
            under.kind = UiScreenKind::Underage;
            once(under);
            return {"", offline(UiOfflineReason::Underage)};
        }
        if (!answer.accepted)
        {
            problem = AgeProblem::Accept;
            continue;
        }
        return {birthDate, std::nullopt};
    }
}

/// Until the 13th birthday the age screen is not shown again; at least until tomorrow when the server and this
/// device's clock disagree.
void UiController::rememberUnderage(const std::string& birthDate)
{
    const std::string until = turnsThirteen(birthDate);
    const std::string today = utcDay(nowMs());
    options_.memory->set(underageKey, Json(until > today ? until : utcDay(nowMs(), 1)).dump());
}

bool UiController::underageNow() const
{
    const std::string until = remembered(options_.memory->get(underageKey));
    return !until.empty() && utcDay(nowMs()) < until;
}

// ── Signing in ──────────────────────────────────────────────────────────

UiResult UiController::signIn(const CancelToken& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(flow_);
    if (!signInOffered()) return offline(UiOfflineReason::Unavailable);
    std::optional<Error> failure;
    try
    {
        switch (options_.signInMode)
        {
        case UiSignInMode::Browser:
            return signedIn(signInBrowser(cancel));
        case UiSignInMode::App:
            return signedIn(client_->signInWithApp(options_.authenticate, cancel));
        default:
            return signedIn(signInDevice(cancel));
        }
    }
    catch (const Cancelled&)
    {
        return cancelled();
    }
    catch (const Error& err)
    {
        failure = err;
    }
    catch (const std::exception&)
    {
        // Not the SDK's own failure: the generic error screen below.
    }
    return failOffline(failure ? errorScreen(*failure, options_.texts, options_.locale, offsetAt(nowMs())) : errorScreen(options_.texts));
}

void UiController::signOut()
{
    client_->signOut();
}

/// The system browser: the waiting screen stays up while the SDK listens for the page's return.
Player UiController::signInBrowser(const CancelToken& external)
{
    CancelToken inner;
    Mailbox box;
    Shower shower(*host_);
    Waiter finisher;
    finisher.start(inner, [this, &box, inner] {
        Event done;
        try
        {
            done.player = client_->signInWithBrowser(
                [this, &box](const std::string& address) {
                    options_.openUrl(address);
                    Event opened;
                    opened.kind = Event::Kind::Opened;
                    opened.url = address;
                    box.post(std::move(opened));
                },
                inner);
            done.kind = Event::Kind::Signed;
        }
        catch (...)
        {
            done.kind = Event::Kind::Failed;
            done.error = std::current_exception();
        }
        box.post(std::move(done));
    });
    std::string url;
    const auto showWaiting = [this, &box, &shower, &url] {
        UiScreen screen;
        screen.kind = UiScreenKind::Waiting;
        screen.url = url;
        shower.start([this, &box, screen](const CancelToken& dismiss) {
            Event answered;
            answered.answer = host_->show(screen, dismiss);
            box.post(std::move(answered));
        });
    };
    for (;;)
    {
        if (external.cancelled()) inner.cancel();
        const auto event = box.next(std::chrono::milliseconds(50));
        if (!event) continue;
        switch (event->kind)
        {
        case Event::Kind::Opened:
            url = event->url;
            showWaiting();
            break;
        case Event::Kind::Answered:
            if (event->answer.kind == UiAnswerKind::Reopen)
            {
                options_.openUrl(url);
                showWaiting();
            }
            else if (event->answer.kind == UiAnswerKind::Cancel)
            {
                inner.cancel();
            }
            break;
        case Event::Kind::Signed:
            return *event->player;
        case Event::Kind::Failed:
            std::rethrow_exception(event->error);
        }
    }
}

UiScreen UiController::deviceScreen(const DeviceLink& link, bool expired) const
{
    UiScreen screen;
    screen.kind = UiScreenKind::Device;
    screen.code = link.userCode;
    screen.uri = shortAddress(link.verificationUri);
    screen.uriComplete = link.verificationUriComplete.empty() ? link.verificationUri : link.verificationUriComplete;
    try
    {
        screen.qr = qrModules(screen.uriComplete);
    }
    catch (const std::length_error&)
    {
        // An address too long for the QR code: the code and the short address are on the screen all the same.
    }
    screen.expired = expired;
    return screen;
}

/// A console's or TV's sign-in: a code, an address and a QR; a new code replaces the old on the same screen.
Player UiController::signInDevice(const CancelToken& external)
{
    Mailbox box;
    Shower shower(*host_);
    Waiter finisher;
    int generation = 0;
    DeviceLink link;
    UiScreen screen;
    const auto begin = [&](const DeviceLink& next) {
        link = next;
        ++generation;
        screen = deviceScreen(link, false);
        screen.expiresAtMs = nowMs() + static_cast<std::int64_t>(link.expiresIn) * 1000;
        shower.start([this, &box, shown = screen, generation](const CancelToken& dismiss) {
            Event answered;
            answered.generation = generation;
            answered.answer = host_->show(shown, dismiss);
            box.post(std::move(answered));
        });
        const CancelToken token;
        finisher.start(token, [this, &box, token, next, generation] {
            Event done;
            done.generation = generation;
            try
            {
                done.player = client_->finishDeviceLink(next, token);
                done.kind = Event::Kind::Signed;
            }
            catch (...)
            {
                done.kind = Event::Kind::Failed;
                done.error = std::current_exception();
            }
            box.post(std::move(done));
        });
    };
    begin(client_->startDeviceLink(options_.deviceName));
    for (;;)
    {
        if (external.cancelled())
        {
            finisher.stop();
            throw Cancelled();
        }
        const auto event = box.next(std::chrono::milliseconds(50));
        if (!event || event->generation != generation) continue;
        switch (event->kind)
        {
        case Event::Kind::Answered:
            if (event->answer.kind == UiAnswerKind::Cancel)
            {
                finisher.stop();
                throw Cancelled();
            }
            if (event->answer.kind == UiAnswerKind::NewCode)
            {
                finisher.stop();
                begin(client_->startDeviceLink(options_.deviceName));
            }
            break;
        case Event::Kind::Signed:
            return *event->player;
        case Event::Kind::Failed:
            try
            {
                std::rethrow_exception(event->error);
            }
            catch (const Error& err)
            {
                if (err.code() != "expired_token") throw;
                screen.expired = true;
                host_->update(screen);
            }
            break;
        case Event::Kind::Opened:
            break;
        }
    }
}

// ── Saves ───────────────────────────────────────────────────────────────

SaveChoice UiController::save(const std::string& slot, const std::string& bytes, const std::optional<std::string>& base, const std::string& name)
{
    std::optional<FileVersion> current;
    try
    {
        SaveChoice saved;
        saved.version = client_->saveFile(slot, bytes, base);
        return saved;
    }
    catch (const Error& err)
    {
        if (err.code() != "file_conflict" || !err.details()["current"].isObject()) throw;
        current = FileVersion::fromJson(err.details()["current"]);
    }
    return chooseSave(slot, bytes, *current, name);
}

SaveCard UiController::card(const std::string& label, const std::string& savedAt, std::int64_t sizeBytes, const std::string& slot, const std::optional<std::string>& bytes) const
{
    SaveCard made;
    made.label = label;
    if (bytes && options_.describeSave)
    {
        try
        {
            made.summary = options_.describeSave(slot, *bytes);
        }
        catch (...)
        {
            made.summary.reset();
        }
    }
    const std::int64_t now = nowMs();
    const auto at = detail::parseTimeMs(savedAt);
    made.when = savedWhen(savedAt, now, offsetAt(at.value_or(now)), offsetAt(now), options_.locale, options_.texts);
    made.size = fileSize(sizeBytes);
    return made;
}

std::optional<std::string> UiController::playerSubject() const
{
    const auto player = client_->player();
    if (!player) return std::nullopt;
    return player->subject;
}

void UiController::assertSamePlayer(const std::optional<std::string>& owner) const
{
    if (playerSubject() != owner) throw Error("context_changed", "The player changed while the save screen was open; nothing was saved.");
}

SaveChoice UiController::chooseSave(const std::string& slot, const std::string& bytes, const FileVersion& current, const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(flow_);
    FileVersion seen = current;
    const auto owner = playerSubject();
    try
    {
        for (;;)
        {
            const auto read = client_->readFile(slot);
            assertSamePlayer(owner);
            if (!read || !read->versionId)
            {
                SaveChoice saved;
                saved.version = client_->saveFile(slot, bytes, std::nullopt);
                return saved;
            }
            FileVersion there = seen;
            there.id = *read->versionId;
            there.generation = read->generation;
            there.savedAt = read->savedAt;
            there.sizeBytes = static_cast<std::int64_t>(read->bytes.size());
            const std::int64_t now = nowMs();
            SaveCard hereCard = card(t("conflict.here"), detail::isoFromMs(now), static_cast<std::int64_t>(bytes.size()), slot, bytes);
            const std::string thereLabel = there.platform ? t("conflict.there") + " \xC2\xB7 " + platformName(*there.platform) : t("conflict.there");
            SaveCard thereCard = card(thereLabel, there.savedAt, there.sizeBytes, slot, read->bytes);
            const auto thereMs = detail::parseTimeMs(there.savedAt);
            const bool hereNewer = thereMs && now >= *thereMs;
            hereCard.newer = hereNewer;
            thereCard.newer = !hereNewer;
            UiScreen screen;
            screen.kind = UiScreenKind::Conflict;
            screen.slot = name.empty() ? slot : name;
            screen.cards = {hereCard, thereCard};
            const UiAnswer answer = once(screen);
            assertSamePlayer(owner);
            if (answer.kind == UiAnswerKind::Keep && answer.card == 1)
            {
                SaveChoice kept;
                kept.keptThere = true;
                kept.version = there;
                kept.bytes = read->bytes;
                return kept;
            }
            try
            {
                SaveChoice saved;
                saved.version = client_->saveFile(slot, bytes, there.id);
                return saved;
            }
            catch (const Error& err)
            {
                if (err.code() != "file_conflict" || !err.details()["current"].isObject()) throw;
                seen = FileVersion::fromJson(err.details()["current"]);
            }
        }
    }
    catch (const HostClosed&)
    {
        throw Error("file_conflict", "Another device saved this slot first and nobody could be asked which save to keep.", 409);
    }
}

std::vector<ResolvedSave> UiController::resolveMergedSaves()
{
    std::lock_guard<std::recursive_mutex> lock(flow_);
    std::vector<ResolvedSave> out;
    const auto owner = playerSubject();
    try
    {
        const auto listed = client_->listFiles();
        assertSamePlayer(owner);
        for (const SlotView& slot : listed.slots)
        {
            if (!slot.conflict || !slot.file) continue;
            const auto read = client_->readFile(slot.key);
            assertSamePlayer(owner);
            const FileVersion& waiting = *slot.conflict;
            const std::string waitingLabel = waiting.platformCloud ? t("conflict.cloud", {{"platform", platformName(*waiting.platformCloud)}}) : t("conflict.merged");
            SaveCard currentCard = card(t("conflict.current"), slot.file->savedAt, slot.file->sizeBytes, slot.key, read ? std::optional<std::string>(read->bytes) : std::nullopt);
            SaveCard waitingCard = card(waitingLabel, waiting.savedAt, waiting.sizeBytes, slot.key, std::nullopt);
            const auto waitingMs = detail::parseTimeMs(waiting.savedAt);
            const auto currentMs = detail::parseTimeMs(slot.file->savedAt);
            const bool waitingNewer = waitingMs && currentMs && *waitingMs > *currentMs;
            currentCard.newer = !waitingNewer;
            waitingCard.newer = waitingNewer;
            UiScreen screen;
            screen.kind = UiScreenKind::Conflict;
            screen.slot = slot.name.empty() ? slot.key : slot.name;
            screen.cards = {currentCard, waitingCard};
            const UiAnswer answer = once(screen);
            assertSamePlayer(owner);
            const FileVersion& keep = answer.kind == UiAnswerKind::Keep && answer.card == 1 ? waiting : *slot.file;
            out.push_back({slot.key, client_->resolveFile(slot.key, keep.id, slot.file->id)});
        }
    }
    catch (const HostClosed&)
    {
        // Nobody could be asked: the slots not resolved yet keep waiting for the next time.
    }
    return out;
}

void UiController::resolvePlatformSaveConflict(const PlatformSaveConflict& conflict)
{
    if (!options_.platformSaveConflicts) return;
    // The copy belongs to the player it was found for: another player never sees it nor answers it (the copy
    // keeps waiting on the server for its player).
    const auto owner = playerSubject();
    std::lock_guard<std::recursive_mutex> lock(flow_);
    try
    {
        if (playerSubject() != owner) return;
        const auto read = client_->readFile(conflict.slot);
        if (playerSubject() != owner) return;
        const std::string platform = conflict.platform.platformCloud.value_or(conflict.platform.platform.value_or(""));
        SaveCard serverCard = card(t("conflict.current"), conflict.server.savedAt, conflict.server.sizeBytes, conflict.slot, read ? std::optional<std::string>(read->bytes) : std::nullopt);
        SaveCard platformCard = card(t("conflict.cloud", {{"platform", platformName(platform)}}), conflict.platform.savedAt, conflict.platform.sizeBytes, conflict.slot, conflict.platformData);
        const auto platformMs = detail::parseTimeMs(conflict.platform.savedAt);
        const auto serverMs = detail::parseTimeMs(conflict.server.savedAt);
        const bool platformNewer = platformMs && serverMs && *platformMs > *serverMs;
        serverCard.newer = !platformNewer;
        platformCard.newer = platformNewer;
        UiScreen screen;
        screen.kind = UiScreenKind::Conflict;
        screen.slot = conflict.slot;
        screen.cards = {serverCard, platformCard};
        const UiAnswer answer = once(screen);
        if (playerSubject() != owner) return;
        const FileVersion& keep = answer.kind == UiAnswerKind::Keep && answer.card == 1 ? conflict.platform : conflict.server;
        client_->resolveFile(conflict.slot, keep.id, conflict.server.id);
    }
    catch (const HostClosed&)
    {
        // Nobody could be asked: the copy keeps waiting on the server.
    }
}

// ── The link reminder ───────────────────────────────────────────────────

void UiController::remind()
{
    if (!options_.linkReminder) return;
    const auto player = client_->player();
    if (!player || player->kind != PlayerKind::Guest || reminding_.exchange(true)) return;
    struct Reset
    {
        std::atomic<bool>& flag;
        ~Reset() { flag.store(false); }
    } reset{reminding_};
    {
        // Another screen flow is up: the prompt would only pile on it.
        std::unique_lock<std::recursive_mutex> lock(flow_, std::try_to_lock);
        if (!lock.owns_lock()) return;
    }
    if (host_->remind() == UiReminderAnswer::Link) signIn();
}
}  // namespace krtu
