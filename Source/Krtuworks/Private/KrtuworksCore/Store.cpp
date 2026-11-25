#include "KrtuworksCore/Store.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/Error.h"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace krtu
{
namespace
{
namespace fs = std::filesystem;

/// A UTF-8 string as a path: std::string is read in the ANSI code page on Windows, so a user folder with
/// non-ASCII letters would not be found without the explicit UTF-8 route.
fs::path storePathOf(const std::string& utf8)
{
#if defined(__cpp_char8_t)
    std::u8string text;
    text.reserve(utf8.size());
    for (const char c : utf8) text.push_back(static_cast<char8_t>(c));
    return fs::path(text);
#else
    return fs::u8path(utf8);
#endif
}

std::string storeUtf8Of(const fs::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool storeStartsWith(const std::string& text, const std::string& prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string storeLittleEndian32(std::size_t value)
{
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xff);
    return out;
}

[[noreturn]] void storeFail(const std::string& what, const std::error_code& ec)
{
    throw Error("storage_error", what + ": " + ec.message());
}
}  // namespace

// ── MemoryStore ─────────────────────────────────────────────────────────

std::optional<std::string> MemoryStore::get(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = data_.find(key);
    if (found == data_.end()) return std::nullopt;
    return found->second;
}

void MemoryStore::set(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = value;
}

void MemoryStore::remove(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.erase(key);
}

std::vector<std::string> MemoryStore::keys(const std::string& prefix)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& entry : data_)
    {
        if (storeStartsWith(entry.first, prefix)) out.push_back(entry.first);
    }
    return out;
}

// ── StdFileSystem ───────────────────────────────────────────────────────

StdFileSystem::StdFileSystem(std::string directory) : directory_(std::move(directory))
{
    std::error_code ec;
    const auto dir = storePathOf(directory_);
    fs::create_directories(dir, ec);
    if (ec) storeFail("cannot make " + directory_, ec);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
}

std::optional<std::string> StdFileSystem::read(const std::string& name)
{
    std::ifstream in(storePathOf(directory_) / storePathOf(name), std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void StdFileSystem::writeAtomic(const std::string& name, const std::string& bytes, bool ownerOnly)
{
    const auto dir = storePathOf(directory_);
    const auto target = dir / storePathOf(name);
    const auto temp = dir / storePathOf("." + toHex(randomBytes(16)) + ".tmp");
    std::error_code ec;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) storeFail("cannot write " + name, std::make_error_code(std::errc::io_error));
        // The mode is set before the content is in the file, so a secret is never readable by others.
        if (ownerOnly) fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out)
        {
            out.close();
            fs::remove(temp, ec);
            storeFail("cannot write " + name, std::make_error_code(std::errc::io_error));
        }
    }
    fs::rename(temp, target, ec);
    if (ec)
    {
        std::error_code ignored;
        fs::remove(temp, ignored);
        storeFail("cannot move " + name + " into place", ec);
    }
}

void StdFileSystem::remove(const std::string& name)
{
    std::error_code ec;
    fs::remove(storePathOf(directory_) / storePathOf(name), ec);
    if (ec) storeFail("cannot delete " + name, ec);
}

std::vector<std::string> StdFileSystem::list()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(storePathOf(directory_), ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code typeError;
        if (it->is_regular_file(typeError)) out.push_back(storeUtf8Of(it->path().filename()));
    }
    return out;
}

// ── FileStore ───────────────────────────────────────────────────────────

FileStore::FileStore(std::shared_ptr<FileSystem> files) : files_(std::move(files))
{
    if (!files_) throw std::invalid_argument("a file store needs a file system");
}

std::string FileStore::fileNameOf(const std::string& key)
{
    return base64Url(key) + ".v";
}

std::optional<std::string> FileStore::get(const std::string& key)
{
    return files_->read(fileNameOf(key));
}

void FileStore::set(const std::string& key, const std::string& value)
{
    files_->writeAtomic(fileNameOf(key), value, true);
}

void FileStore::remove(const std::string& key)
{
    files_->remove(fileNameOf(key));
}

std::vector<std::string> FileStore::keys(const std::string& prefix)
{
    std::vector<std::string> out;
    for (const auto& name : files_->list())
    {
        if (name.size() < 2 || name.compare(name.size() - 2, 2, ".v") != 0) continue;
        try
        {
            auto key = fromBase64Url(std::string_view(name).substr(0, name.size() - 2));
            if (storeStartsWith(key, prefix)) out.push_back(std::move(key));
        }
        catch (const std::invalid_argument&)
        {
            // Not one of the store's files.
        }
    }
    return out;
}

// ── SealedFileStore ─────────────────────────────────────────────────────

SealedFileStore::SealedFileStore(std::shared_ptr<FileSystem> files, std::shared_ptr<MasterKey> master) : FileStore(std::move(files)), master_(std::move(master))
{
    if (!master_) throw std::invalid_argument("a sealed store needs a master key");
}

bool SealedFileStore::loadKeys()
{
    std::lock_guard<std::mutex> lock(keysMutex_);
    if (!encKey_.empty()) return true;
    const auto key = master_->getOrCreate();
    if (!key || key->size() != 32) return false;
    encKey_ = hmacSha256(*key, "krtuworks-sealed-file:enc");
    macKey_ = hmacSha256(*key, "krtuworks-sealed-file:mac");
    return true;
}

std::string SealedFileStore::tagOf(const std::string& key, const std::string& iv, const std::string& ciphertext) const
{
    return hmacSha256(macKey_, storeLittleEndian32(key.size()) + key + iv + ciphertext);
}

void SealedFileStore::set(const std::string& key, const std::string& value)
{
    if (!loadKeys()) throw Error("secret_storage_unavailable", "The platform's secret storage is not available.");
    const auto iv = randomBytes(16);
    const auto ciphertext = aes256CbcEncrypt(encKey_, iv, value);
    files_->writeAtomic(fileNameOf(key), iv + ciphertext + tagOf(key, iv, ciphertext), true);
}

std::optional<std::string> SealedFileStore::get(const std::string& key)
{
    const auto name = fileNameOf(key);
    const auto sealedBytes = files_->read(name);
    if (!sealedBytes) return std::nullopt;
    // The platform's storage not answering now (a locked Keychain) is not a reason to lose the secret.
    if (!loadKeys()) return std::nullopt;
    if (sealedBytes->size() < 16 + 16 + 32)
    {
        files_->remove(name);
        return std::nullopt;
    }
    const auto iv = sealedBytes->substr(0, 16);
    const auto ciphertext = sealedBytes->substr(16, sealedBytes->size() - 48);
    const auto tag = sealedBytes->substr(sealedBytes->size() - 32);
    if (!fixedTimeEquals(tagOf(key, iv, ciphertext), tag))
    {
        files_->remove(name);
        return std::nullopt;
    }
    auto plain = aes256CbcDecrypt(encKey_, iv, ciphertext);
    if (!plain) files_->remove(name);
    return plain;
}

// ── MachineBoundMasterKey ───────────────────────────────────────────────

MachineBoundMasterKey::MachineBoundMasterKey(std::string appId, std::shared_ptr<FileSystem> files, std::string fallbackName)
    : appId_(std::move(appId)), files_(std::move(files)), fallbackName_(std::move(fallbackName))
{
}

std::optional<std::string> MachineBoundMasterKey::getOrCreate()
{
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"})
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::string machine(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
        while (!machine.empty() && (machine.back() == '\n' || machine.back() == '\r' || machine.back() == ' ' || machine.back() == '\t')) machine.pop_back();
        if (machine.empty()) continue;
#if defined(__unix__) || defined(__APPLE__)
        const std::string user = std::to_string(static_cast<unsigned long>(getuid()));
#else
        const std::string user = "no-uid";
#endif
        return hmacSha256("krtuworks-sdk:machine-bound-key", machine + "\n" + user + "\n" + appId_);
    }
    if (files_)
    {
        const auto kept = files_->read(fallbackName_);
        if (kept && kept->size() == 32) return kept;
        auto key = randomBytes(32);
        files_->writeAtomic(fallbackName_, key, true);
        return key;
    }
    return std::nullopt;
}

// ── The Android key file ────────────────────────────────────────────────

std::optional<std::string> resolveKeystoreKey(FileSystem& files, const std::string& name,
                                              const std::function<std::optional<std::string>(const std::string&)>& open,
                                              const std::function<std::optional<std::string>(const std::string&)>& seal)
{
    const std::string plainPrefix = keystorePlainPrefix;
    if (const auto kept = files.read(name))
    {
        const bool plain = kept->compare(0, plainPrefix.size(), plainPrefix) == 0;
        const std::optional<std::string> opened = plain ? std::optional<std::string>(kept->substr(plainPrefix.size())) : open(*kept);
        if (!opened) return std::nullopt;
        if (!opened->empty())
        {
            if (plain)
            {
                // The same key, sealed now; the file stays as it was when sealing still fails or it cannot be written.
                const auto sealedKey = seal(*opened);
                if (sealedKey && !sealedKey->empty())
                {
                    try
                    {
                        files.writeAtomic(name, *sealedKey, true);
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            }
            return fromBase64(*opened);
        }
    }
    const std::string key = randomBytes(32);
    const std::string encoded = base64(key);
    const auto sealedKey = seal(encoded);
    files.writeAtomic(name, sealedKey && !sealedKey->empty() ? *sealedKey : plainPrefix + encoded, true);
    return key;
}
}  // namespace krtu
