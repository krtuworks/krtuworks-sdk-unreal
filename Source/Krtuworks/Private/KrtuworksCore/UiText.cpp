#include "KrtuworksCore/UiText.h"

namespace krtu
{
namespace
{
const std::string& templateFor(const UiTexts& texts, const std::string& key)
{
    const auto own = texts.find(key);
    if (own != texts.end()) return own->second;
    const auto& defaults = uiDefaultTexts();
    const auto found = defaults.find(key);
    return found != defaults.end() ? found->second : key;
}

bool isMarkLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// The end of the `{name}` mark that starts at `at` (one past its closing brace), or `at` when none starts there.
std::size_t markEnd(const std::string& text, std::size_t at)
{
    if (text[at] != '{') return at;
    std::size_t i = at + 1;
    while (i < text.size() && isMarkLetter(text[i])) ++i;
    return i > at + 1 && i < text.size() && text[i] == '}' ? i + 1 : at;
}
}  // namespace

const UiTexts& uiDefaultTexts()
{
    static const UiTexts table = {
        {"age.title", "When were you born?"},
        {"age.day", "Day"},
        {"age.month", "Month"},
        {"age.year", "Year"},
        {"age.accept", "I accept the {terms} and the {privacy}."},
        {"age.termsLink", "Terms of Service"},
        {"age.privacyLink", "Privacy Notice"},
        {"age.continue", "Continue"},
        {"age.signIn", "Sign in with Krtuworks ID"},
        {"age.invalid", "Enter your date of birth."},
        {"age.acceptNeeded", "Accept the terms to continue."},
        {"month.1", "January"},
        {"month.2", "February"},
        {"month.3", "March"},
        {"month.4", "April"},
        {"month.5", "May"},
        {"month.6", "June"},
        {"month.7", "July"},
        {"month.8", "August"},
        {"month.9", "September"},
        {"month.10", "October"},
        {"month.11", "November"},
        {"month.12", "December"},
        {"terms.title", "Our terms changed"},
        {"terms.body", "Accept the current {terms} and {privacy} to keep playing online."},
        {"terms.accept", "Accept"},
        {"underage.title", "Online play needs you to be 13"},
        {"underage.body", "You can keep playing on this device. Progress stays here and isn't saved online."},
        {"underage.play", "Play offline"},
        {"signIn.button", "Sign in with Krtuworks ID"},
        {"account.guest", "Guest"},
        {"account.label", "Krtuworks ID"},
        {"account.manage", "Manage account"},
        {"account.link", "Link a Krtuworks ID"},
        {"account.signOut", "Sign out"},
        {"conflict.title", "Two saves for {slot}"},
        {"conflict.body", "Pick the one to keep. The other stays in your save history."},
        {"conflict.here", "This device"},
        {"conflict.there", "Another device"},
        {"conflict.current", "Current save"},
        {"conflict.cloud", "{platform} Cloud"},
        {"conflict.merged", "From your other account"},
        {"conflict.newer", "Newer"},
        {"conflict.keep", "Keep this save"},
        {"reminder.title", "Keep your progress safe"},
        {"reminder.body", "Link a Krtuworks ID so a new phone or a reinstall doesn't lose it."},
        {"reminder.later", "Later"},
        {"reminder.link", "Link"},
        {"error.title", "Couldn't sign in"},
        {"error.offline", "No connection to Krtuworks. Your game keeps going; we'll try again when you're back online."},
        {"error.cancelled", "Sign-in was cancelled."},
        {"error.banned", "You can't play this game online."},
        {"error.bannedReason", "You can't play this game online: {reason}"},
        {"error.bannedUntil", "This ends on {date}."},
        {"error.maintenance", "Online features are down for maintenance."},
        {"error.update", "A new version is out. Update the game to keep playing online."},
        {"error.busy", "Too many new players right now. Try again in a moment."},
        {"error.generic", "Something went wrong. Try again in a moment."},
        {"error.retry", "Try again"},
        {"error.continue", "Keep playing"},
        {"waiting.title", "Finish signing in in your browser"},
        {"waiting.body", "We opened id.krtuworks.com. Come back here when it says you're done."},
        {"waiting.reopen", "Open the page again"},
        {"waiting.cancel", "Cancel"},
        {"device.title", "Sign in on your phone"},
        {"device.go", "Go to {uri}"},
        {"device.enter", "Enter this code"},
        {"device.waiting", "Waiting \xC2\xB7 code works for {time}"},
        {"device.expired", "This code expired."},
        {"device.newCode", "New code"},
        {"device.cancel", "Cancel"},
        {"hint.select", "Select"},
        {"hint.back", "Back"},
        {"hint.selectButton", "A"},
        {"hint.backButton", "B"},
        {"time.today", "Today, {time}"},
        {"time.yesterday", "Yesterday, {time}"},
        {"platform.STEAM", "Steam"},
        {"platform.EPIC", "Epic Games"},
        {"platform.WINDOWS", "Windows"},
        {"platform.MACOS", "Mac"},
        {"platform.LINUX", "Linux"},
        {"platform.IOS", "iPhone or iPad"},
        {"platform.ANDROID", "Android"},
        {"platform.WEB", "Browser"},
        {"platform.XBOX", "Xbox"},
        {"platform.PLAYSTATION", "PlayStation"},
        {"platform.NINTENDO", "Nintendo Switch"},
    };
    return table;
}

std::string textFor(const UiTexts& texts, const std::string& key, const std::map<std::string, std::string>& vars)
{
    std::string out;
    for (const auto& part : textSegments(texts, key))
    {
        if (!part.mark)
        {
            out += part.text;
            continue;
        }
        const auto value = vars.find(part.text);
        out += value != vars.end() ? value->second : "{" + part.text + "}";
    }
    return out;
}

std::vector<TextSegment> textSegments(const UiTexts& texts, const std::string& key)
{
    const std::string& text = templateFor(texts, key);
    std::vector<TextSegment> out;
    std::string plain;
    std::size_t at = 0;
    while (at < text.size())
    {
        const std::size_t end = markEnd(text, at);
        if (end == at)
        {
            plain += text[at++];
            continue;
        }
        if (!plain.empty()) out.push_back({plain, false});
        plain.clear();
        out.push_back({text.substr(at + 1, end - at - 2), true});
        at = end;
    }
    if (!plain.empty()) out.push_back({plain, false});
    return out;
}
}  // namespace krtu
