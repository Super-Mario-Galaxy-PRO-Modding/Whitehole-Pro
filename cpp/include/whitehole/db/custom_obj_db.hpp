#pragma once

// CustomObjDatabase: the registry of *custom* objects (names the community
// objectdb.json does not know) so the editor can preview them.
//
// Why this exists: the Blender exporter lets modders ship brand-new objects,
// but a new object name placed in a BCSV is invisible to every part of the
// editor that consults objectdb.json. The BCSV editor's object section keeps
// this database in step automatically -- a name added to an object table that
// the official database does not know is registered here, and a registered
// name that disappears from the table that registered it is dropped again.
// ModelLibrary then resolves those names through CustomObjectEntry::modelPath
// so the viewport draws the exported model instead of a placeholder.
//
// Entries remember which BCSV source registered them, so syncing one table can
// never delete an entry another table added.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whitehole::db {

class ObjectDatabase;

struct CustomObjectEntry {
    std::string name;        // internal object name (BCSV "name" value)
    std::string displayName; // optional friendly label; empty = use name
    std::string modelPath;   // preview model: "/..." inside the workspace,
                             // an absolute path, or empty for no preview
    std::string notes;
    std::string source;      // BCSV that registered this entry (see sync)
    std::int64_t addedAt{0}; // unix seconds, informational only
};

// What one sync pass changed. `added`/`removed` hold object names, as does
// `adopted` (entries that existed without a source and gained one).
struct CustomObjSyncOutcome {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> adopted;

    [[nodiscard]] bool changed() const noexcept {
        return !added.empty() || !removed.empty() || !adopted.empty();
    }
};

class CustomObjDatabase {
public:
    // Missing file == empty database; malformed file throws std::runtime_error
    // so a corrupt registry never silently loses modder data.
    void load(const std::filesystem::path& path);
    void loadFromJson(std::string_view text);
    // Writes only when the in-memory content differs from what was loaded, so
    // an idle editor never touches the file's timestamp.
    void save(const std::filesystem::path& path);
    [[nodiscard]] std::string toJson() const;
    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool contains(std::string_view name) const;
    // nullptr when unknown. The pointer stays valid until the next mutation.
    [[nodiscard]] const CustomObjectEntry* find(std::string_view name) const;
    // Insertion order (stable across save/load) for deterministic UI + JSON.
    [[nodiscard]] const std::vector<CustomObjectEntry>& entries() const noexcept { return entries_; }

    // Inserts (or overwrites by name) and returns true when the database changed.
    bool add(CustomObjectEntry entry);
    // Returns true when an entry was dropped.
    bool remove(std::string_view name);
    // Sets/ clears the preview model path of an existing entry (creates the
    // entry when it is missing so "assign a model" works before first sync).
    bool setModelPath(std::string_view name, std::string modelPath);

    // Reconciles one object section against this database:
    //  - every name in `namesPresent` that `official` does not know is ensured
    //    to exist here (recorded under `source`),
    //  - every entry registered by `source` whose name is no longer present is
    //    removed,
    //  - names `official` knows are never stored (they are not custom).
    // When `official` is null or empty nothing is added -- without the vanilla
    // database there is no way to tell a custom object from a shipped one, and
    // guessing would flood this file with thousands of vanilla names.
    // Removals still run, so a stale source cleans itself up regardless.
    [[nodiscard]] CustomObjSyncOutcome syncFromObjectSection(
        std::string_view source, const std::vector<std::string>& namesPresent,
        const ObjectDatabase* official);

private:
    void reindex() noexcept;

    std::vector<CustomObjectEntry> entries_;
    std::unordered_map<std::string, std::size_t> lookup_;
    // Serialized form at the last load/save; save() skips the write when the
    // document is unchanged.
    std::string lastSavedJson_;
    bool loaded_{false};
};

} // namespace whitehole::db
