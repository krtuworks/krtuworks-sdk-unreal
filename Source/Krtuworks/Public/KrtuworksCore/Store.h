#pragma once

#include "KrtuworksCore/Export.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace krtu
{
/// Where the SDK keeps what must outlive the app: queues, the remote config, meta (a store), and the
/// secrets (tokens, the device key) in a second store backed by the platform's secret storage. Values are
/// JSON text. Implementations are called from several threads at once.
class KRTU_API KeyValueStore
{
public:
    virtual ~KeyValueStore() = default;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual void remove(const std::string& key) = 0;
    virtual std::vector<std::string> keys(const std::string& prefix) = 0;
};

/// Kept in this process only (tests, tools).
class KRTU_API MemoryStore : public KeyValueStore
{
public:
    std::optional<std::string> get(const std::string& key) override;
    void set(const std::string& key, const std::string& value) override;
    void remove(const std::string& key) override;
    std::vector<std::string> keys(const std::string& prefix) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::string> data_;
};

/// One folder of files as the file stores need it. The Unreal layer gives the engine's file system (a
/// folder under the game's saved data); StdFileSystem is the plain C++ one.
class KRTU_API FileSystem
{
public:
    virtual ~FileSystem() = default;
    /// The whole file `name`, or nothing when there is no such file.
    virtual std::optional<std::string> read(const std::string& name) = 0;
    /// Writes `bytes` as `name` through a temporary file moved into place: a crash leaves the old content
    /// or the new, never half. `ownerOnly`: readable by this user only (0600) where the system has modes.
    virtual void writeAtomic(const std::string& name, const std::string& bytes, bool ownerOnly) = 0;
    virtual void remove(const std::string& name) = 0;
    /// The names of the files in the folder.
    virtual std::vector<std::string> list() = 0;
};

/// A folder through std::filesystem. `directory` is UTF-8; it is made (owner-only where the system has modes).
class KRTU_API StdFileSystem : public FileSystem
{
public:
    explicit StdFileSystem(std::string directory);
    std::optional<std::string> read(const std::string& name) override;
    void writeAtomic(const std::string& name, const std::string& bytes, bool ownerOnly) override;
    void remove(const std::string& name) override;
    std::vector<std::string> list() override;

private:
    std::string directory_;
};

/// The store as files: one file per key (the name is the key in base64url, so any key is a file name, never
/// a path), written atomically.
class KRTU_API FileStore : public KeyValueStore
{
public:
    explicit FileStore(std::shared_ptr<FileSystem> files);
    std::optional<std::string> get(const std::string& key) override;
    void set(const std::string& key, const std::string& value) override;
    void remove(const std::string& key) override;
    std::vector<std::string> keys(const std::string& prefix) override;

protected:
    static std::string fileNameOf(const std::string& key);
    std::shared_ptr<FileSystem> files_;
};

/// Where the 32-byte key that seals the secret files lives (the platform's own secret storage).
class KRTU_API MasterKey
{
public:
    virtual ~MasterKey() = default;
    /// The key, made and kept on the first call; nothing when the platform's storage is not available now.
    virtual std::optional<std::string> getOrCreate() = 0;
};

/// The secrets (tokens, the device key) as sealed files: AES-256-CBC, then HMAC-SHA256 over the key name,
/// the IV and the ciphertext (a file moved to another key or another device does not open). The sealing key
/// comes from the platform (MasterKey). A file sealed with another key (another device's, restored from a
/// backup without its key) is deleted and reads as missing: the SDK starts over as a new device.
/// When the platform does not give the key right now, nothing is deleted: reads are empty and
/// writes are refused (`secret_storage_unavailable`) until it does.
class KRTU_API SealedFileStore : public FileStore
{
public:
    SealedFileStore(std::shared_ptr<FileSystem> files, std::shared_ptr<MasterKey> master);
    std::optional<std::string> get(const std::string& key) override;
    void set(const std::string& key, const std::string& value) override;

private:
    bool loadKeys();
    std::string tagOf(const std::string& key, const std::string& iv, const std::string& ciphertext) const;

    std::shared_ptr<MasterKey> master_;
    std::mutex keysMutex_;
    std::string encKey_;
    std::string macKey_;
};

/// Linux and Steam Deck (the owner chose one way for desktop and game mode alike): the sealing key is bound
/// to this machine, this user and this app (`/etc/machine-id`, the uid, the app id), so sealed files copied
/// to another machine or user do not open. A machine without a machine id keeps a random key in an
/// owner-only file of `files` instead.
class KRTU_API MachineBoundMasterKey : public MasterKey
{
public:
    MachineBoundMasterKey(std::string appId, std::shared_ptr<FileSystem> files, std::string fallbackName = "master.key");
    std::optional<std::string> getOrCreate() override;

private:
    std::string appId_;
    std::shared_ptr<FileSystem> files_;
    std::string fallbackName_;
};
}  // namespace krtu
