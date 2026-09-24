#pragma once

// Object model loading: resolves a placement object's model archive inside the
// game workspace (ObjectData/<Name>.arc), parses the BMD/BDL it contains and
// converts it into the flat triangle mesh the viewport draws. Pure data + I/O:
// no Win32, no OpenGL, so the whole pipeline stays unit-testable.
//
// Mirrors the Java BmdRenderer default load path: the object name runs through
// ModelSubstitutions, the "Low"/"Middle" suffix is appended when the low-poly
// setting is on and that archive exists, and the model inside the archive is
// looked up as /<Name>/<Name>.bdl then .bmd (RARC lookups are case-insensitive).

#include "whitehole/render/model_mesh.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whitehole::io {
class DirectoryFilesystem;
class RarcArchive;
struct RarcEntry;
} // namespace whitehole::io
namespace whitehole::db {
class ModelSubstitutions;
}

namespace whitehole::render {

// Stage-by-stage report for one object's model lookup, produced by
// ModelLibrary::probe(). Lets tools (and the `models check` CLI command)
// explain exactly why an object kept its placeholder shape instead of a
// real game model.
struct ModelProbe {
    std::string objectName;
    std::string archiveName;   // on-disk ObjectData archive used, empty when none
    std::string modelPath;     // BMD/BDL path inside the archive
    std::size_t modelBytes{0};
    bool archiveFound{false};
    bool modelFound{false};
    bool parsed{false};        // BMD parse succeeded
    std::size_t sceneNodes{0};
    std::size_t batches{0};
    std::size_t packets{0};    // across all batches (SHP1 decode check)
    std::size_t triangles{0};
    std::size_t skippedPrimitives{0};
    // Emission-loop drops (used to be silent continues in buildModelMesh).
    std::size_t droppedEmptyMatrixTable{0};
    std::size_t droppedBadMatrixIndex{0};
    // Multi-part objects: every companion archive merged into the mesh
    // (numbered PlantA00-style parts, GrapyonBody/Head, ...). Empty for
    // ordinary single-archive objects.
    std::vector<std::string> partArchives;
    std::string error;         // first failing stage; empty when usable

    [[nodiscard]] bool usable() const noexcept { return parsed && triangles != 0; }
};

class ModelLibrary {
public:
    // Binds the game workspace the ObjectData archives live in. Passing
    // nullptr unbinds and drops every cached mesh (e.g. when a map archive is
    // opened without a game directory).
    void bind(const io::DirectoryFilesystem* filesystem);
    [[nodiscard]] bool bound() const noexcept { return filesystem_ != nullptr; }

    // Optional name substitution table (data/modelsubstitutions.json).
    void setSubstitutions(const db::ModelSubstitutions* substitutions) noexcept { substitutions_ = substitutions; }
    // Java's "use low-poly models" setting; toggling rebuilds the caches.
    void setLowPoly(bool lowPoly);
    void clear() noexcept;

    // Mesh for a placement object, or nullptr when the workspace has no model
    // for it (the viewport keeps drawing the placeholder shape then). Results
    // are cached, including misses, so repeated rebuilds stay cheap; the
    // returned pointer stays valid for the lifetime of the library.
    [[nodiscard]] std::shared_ptr<const ModelMesh> model(std::string_view objectName);

    // Test/status hook: the ObjectData archive file name `model()` would use
    // for an object, or an empty string when no archive exists.
    [[nodiscard]] std::string archiveNameFor(std::string_view objectName) const;

    // Diagnostic: run the full model pipeline for one object without touching
    // the caches and report every stage's outcome (archive found, model entry
    // found, BMD parsed, triangles produced). Never throws; failures are
    // recorded in ModelProbe::error.
    [[nodiscard]] ModelProbe probe(std::string_view objectName) const;

    // Status-line bookkeeping: how many distinct names resolved to a real
    // model vs. fell back to the placeholder since the last reset.
    [[nodiscard]] std::size_t loadedCount() const noexcept { return loadedCount_; }
    [[nodiscard]] std::size_t missingCount() const noexcept { return missingCount_; }
    void resetCounters() noexcept;

private:
    struct CacheEntry {
        std::shared_ptr<const ModelMesh> mesh; // null = known missing
    };

    [[nodiscard]] std::shared_ptr<const ModelMesh> loadModel(std::string_view objectName);
    [[nodiscard]] bool archiveExists(std::string_view archiveName) const;
    // Case-insensitive "<stem>.arc" lookup in the ObjectData listing cache
    // (empty when unbound, the listing failed, or the archive is absent).
    [[nodiscard]] std::string lookupArchive(std::string_view stem) const;
    // Object name after ModelSubstitutions (unchanged when no rule applies).
    [[nodiscard]] std::string substitutedCandidate(std::string_view objectName) const;
    // Companion archives for multi-part objects: every on-disk numbered
    // (00..09) / body-part (Body/Wing/Head/Big) variant when the object has
    // no single exact <Name>.arc. Probed against the real listing only.
    [[nodiscard]] std::vector<std::string> variantArchivesFor(std::string_view objectName) const;
    void refreshListing() const;
    void evictIfNeeded();

    const io::DirectoryFilesystem* filesystem_{nullptr};
    const db::ModelSubstitutions* substitutions_{nullptr};
    bool lowPoly_{false};
    // Case-insensitive ObjectData listing cache: lowercase name -> on-disk name.
    mutable std::unordered_map<std::string, std::string> archiveNames_;
    mutable bool listingReady_{false};
    mutable bool listingFailed_{false};
    std::vector<std::string> cacheOrder_; // FIFO eviction order
    std::unordered_map<std::string, CacheEntry> cache_;
    std::size_t loadedCount_{0};
    std::size_t missingCount_{0};
};

// Finds the model file inside an ObjectData archive: /<Stem>/<Stem>.bdl then
// .bmd (the layout the game ships), falling back to the archive's single
// model file when the expected layout is absent. Returns nullptr when the
// archive holds no usable model (multi-model archives are ambiguous).
[[nodiscard]] const io::RarcEntry* findModelEntry(const io::RarcArchive& archive, std::string_view stem);

} // namespace whitehole::render