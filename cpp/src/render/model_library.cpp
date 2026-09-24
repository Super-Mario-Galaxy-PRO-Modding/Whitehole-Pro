#include "whitehole/render/model_library.hpp"

#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/io/directory_filesystem.hpp"
#include "whitehole/io/rarc.hpp"
#include "whitehole/smg/bmd.hpp"
#include "whitehole/util/text.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

namespace whitehole::render {
namespace {

// Memory guard: a whole zone can reference hundreds of distinct models; past
// this many cached meshes the oldest entries are evicted. Meshes still held by
// the viewport scene survive eviction through their shared_ptr.
constexpr std::size_t kMaxCachedMeshes = 256;

} // namespace

void ModelLibrary::bind(const io::DirectoryFilesystem* filesystem) {
    if (filesystem_ == filesystem) {
        return;
    }
    filesystem_ = filesystem;
    clear();
}

void ModelLibrary::setLowPoly(bool lowPoly) {
    if (lowPoly_ == lowPoly) {
        return;
    }
    lowPoly_ = lowPoly;
    // The Low/Middle suffix changes which archive each name resolves to.
    clear();
}

void ModelLibrary::clear() noexcept {
    cache_.clear();
    cacheOrder_.clear();
    archiveNames_.clear();
    listingReady_ = false;
    listingFailed_ = false;
}

void ModelLibrary::resetCounters() noexcept {
    loadedCount_ = 0;
    missingCount_ = 0;
}

void ModelLibrary::refreshListing() const {
    if (listingReady_ || listingFailed_) {
        return;
    }
    listingReady_ = true;
    if (filesystem_ == nullptr) {
        listingFailed_ = true;
        return;
    }
    try {
        for (const auto& file : filesystem_->files("ObjectData")) {
            archiveNames_.emplace(whitehole::util::toLower(file), file);
        }
    } catch (...) {
        // Workspace without an ObjectData directory: no models at all.
        listingFailed_ = true;
    }
}

bool ModelLibrary::archiveExists(std::string_view archiveName) const {
    refreshListing();
    if (listingFailed_) {
        return false;
    }
    return archiveNames_.find(whitehole::util::toLower(archiveName)) != archiveNames_.end();
}
namespace {

// Multi-part model archives: SMG ships some objects as numbered parts
// (PlantA00..PlantA02) or body-part archives (GrapyonBody + GrapyonHead,
// PatakuriWing) instead of one <Name>.arc. Candidates are only ever probed
// against the user's real ObjectData listing, so a name never resolves on a
// dump that lacks it — no guessed data, works on every computer.
std::vector<std::string> variantCandidates(std::string_view base) {
    std::vector<std::string> out;
    out.reserve(14);
    for (int n = 0; n <= 9; ++n) {
        std::string variant(base);
        variant += static_cast<char>('0' + n / 10);
        variant += static_cast<char>('0' + n % 10);
        out.push_back(std::move(variant));
    }
    for (const char* suffix : {"Body", "Wing", "Head", "Big"}) {
        out.push_back(std::string(base) + suffix);
    }
    return out;
}

} // namespace

std::string ModelLibrary::lookupArchive(std::string_view stem) const {
    refreshListing();
    if (listingFailed_) {
        return {};
    }
    const auto found = archiveNames_.find(whitehole::util::toLower(std::string(stem) + ".arc"));
    return found != archiveNames_.end() ? found->second : std::string{};
}

std::string ModelLibrary::substitutedCandidate(std::string_view objectName) const {
    std::string candidate(objectName);
    if (substitutions_ != nullptr && substitutions_->isLoaded()) {
        std::string substitution = substitutions_->substitute(objectName);
        if (!substitution.empty()) {
            candidate = std::move(substitution);
        }
    }
    return candidate;
}

std::string ModelLibrary::archiveNameFor(std::string_view objectName) const {
    if (filesystem_ == nullptr || objectName.empty()) {
        return {};
    }

    refreshListing();
    if (listingFailed_) {
        return {};
    }

    const std::string candidate = substitutedCandidate(objectName);

    // Java ModelSubstitutions.getSubstitutedModelName(): the Low/Middle
    // variants only apply while the low-poly setting is on. Return the
    // on-disk archive name so the case-insensitive listing stays consistent
    // with DirectoryFilesystem::read() on case-sensitive platforms.
    if (lowPoly_) {
        if (auto low = lookupArchive(candidate + "Low"); !low.empty()) {
            return low;
        }
        if (auto middle = lookupArchive(candidate + "Middle"); !middle.empty()) {
            return middle;
        }
    }
    if (auto exact = lookupArchive(candidate); !exact.empty()) {
        return exact;
    }
    // Multi-part / numbered model archives (PlantA00, GrapyonBody, ...):
    // probe part variants against the on-disk listing, candidate first.
    for (const auto& variant : variantCandidates(candidate)) {
        if (auto hit = lookupArchive(variant); !hit.empty()) {
            return hit;
        }
    }
    // A substitution that does not exist on disk falls back to the object's
    // own name, exactly like the Java editor does — including its part variants.
    if (candidate != objectName) {
        if (auto own = lookupArchive(objectName); !own.empty()) {
            return own;
        }
        for (const auto& variant : variantCandidates(objectName)) {
            if (auto hit = lookupArchive(variant); !hit.empty()) {
                return hit;
            }
        }
    }
    return {};
}

std::vector<std::string> ModelLibrary::variantArchivesFor(std::string_view objectName) const {
    // Companions only exist for objects WITHOUT a single exact archive: a
    // present <Name>.arc is a self-contained object, and stacking extra part
    // archives on it would draw unrelated models on the same slot.
    std::vector<std::string> out;
    if (filesystem_ == nullptr || objectName.empty()) {
        return out;
    }
    refreshListing();
    if (listingFailed_) {
        return out;
    }
    const std::string candidate = substitutedCandidate(objectName);
    if (!lookupArchive(candidate).empty() || !lookupArchive(objectName).empty()) {
        return out;
    }
    const auto primaryLower = whitehole::util::toLower(archiveNameFor(objectName));
    auto appendVariants = [&](std::string_view base) {
        for (const auto& variant : variantCandidates(base)) {
            auto hit = lookupArchive(variant);
            if (hit.empty()) {
                continue;
            }
            auto hitLower = whitehole::util::toLower(hit);
            if (hitLower == primaryLower) {
                continue; // the primary archive itself, already loaded
            }
            bool duplicate = false;
            for (const auto& existing : out) {
                if (whitehole::util::toLower(existing) == hitLower) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                out.push_back(std::move(hit));
            }
        }
    };
    appendVariants(candidate);
    if (candidate != objectName) {
        appendVariants(objectName);
    }
    return out;
}

std::shared_ptr<const ModelMesh> ModelLibrary::model(std::string_view objectName) {
    if (filesystem_ == nullptr) {
        return nullptr;
    }
    const std::string key(objectName);
    if (const auto it = cache_.find(key); it != cache_.end()) {
        return it->second.mesh;
    }

    auto mesh = loadModel(objectName);
    if (mesh != nullptr) {
        ++loadedCount_;
    } else {
        ++missingCount_;
    }
    cache_.emplace(key, CacheEntry{mesh});
    cacheOrder_.push_back(key);
    evictIfNeeded();
    return mesh;
}

void ModelLibrary::evictIfNeeded() {
    while (cacheOrder_.size() > kMaxCachedMeshes) {
        const auto oldest = cacheOrder_.front();
        cacheOrder_.erase(cacheOrder_.begin());
        cache_.erase(oldest);
    }
}

const io::RarcEntry* findModelEntry(const io::RarcArchive& archive, std::string_view stem) {
    // Genuine archives ship a directory named after the model holding the model
    // file of the same name; BDL is preferred (it is the pre-baked display list
    // variant the game itself draws). Only the BDL shortcut runs first: a lone
    // stem/stem.bmd still falls through to the ambiguity-counting fallback below
    // so that stem/stem.bmd + Other.bmd is correctly treated as ambiguous.
    // Note: RarcArchive::find() has a fallback that matches bare file names, so
    // we must verify the returned entry actually lives under stem/ and is not
    // just a same-named file at the archive root.
    for (const std::string_view extension : {".bdl"}) {
        const std::string path = std::string(stem) + "/" + std::string(stem) + std::string(extension);
        if (const auto* entry = archive.find(path); entry != nullptr && !entry->directory) {
            // Confirm the entry is really under the stem/ directory (not a bare-name
            // fallback match from the archive root).
            const auto slash = entry->path.rfind('/');
            if (slash != std::string::npos) {
                const auto dir = entry->path.substr(0, slash);
                if (whitehole::util::equalIgnoreCase(dir, stem)) {
                    return entry;
                }
            }
        }
    }

    // Fallback for archives that keep their model in some other layout
    // (root-level file, differently named directory). Archives with genuinely
    // different models (Bar.bmd + Baz.bmd) stay ambiguous and are refused
    // rather than picking an arbitrary one.
    const io::RarcEntry* onlyModel = nullptr;
    std::string onlyStem;
    int modelCount = 0;
    for (const auto& entry : archive.entries()) {
        if (entry.directory) {
            continue;
        }
        const auto path = whitehole::util::toLower(entry.path);
        const bool isBdl = path.size() > 4 && path.compare(path.size() - 4, 4, ".bdl") == 0;
        const bool isBmd = path.size() > 4 && path.compare(path.size() - 4, 4, ".bmd") == 0;
        if (!isBdl && !isBmd) {
            continue;
        }
        const auto slash = entry.path.rfind('/');
        const auto file = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
        const std::string fileStem = whitehole::util::toLower(file.substr(0, file.size() - 4));
        if (onlyModel != nullptr && fileStem == onlyStem) {
            // Same model in both container variants: keep the BDL (preferred).
            if (isBdl) {
                onlyModel = &entry;
            }
            continue;
        }
        onlyModel = &entry;
        onlyStem = fileStem;
        ++modelCount;
    }
    return modelCount == 1 ? onlyModel : nullptr;
}

std::shared_ptr<const ModelMesh> ModelLibrary::loadModel(std::string_view objectName) {
    // One archive -> one mesh. Empty when the archive holds no usable single
    // BMD/BDL entry; a broken part is skipped instead of failing the object.
    const auto meshFromArchive = [this](const std::string& archiveName) {
        ModelMesh mesh;
        if (archiveName.empty()) {
            return mesh;
        }
        const auto bytes = filesystem_->read("/ObjectData/" + archiveName);
        io::RarcArchive archive(bytes);
        const std::string stem = archiveName.substr(0, archiveName.size() - 4); // strip ".arc"
        const auto* entry = findModelEntry(archive, stem);
        if (entry == nullptr) {
            return mesh;
        }
        return buildModelMesh(smg::parseBmd(archive.read(*entry)));
    };

    try {
        const std::string archiveName = archiveNameFor(objectName);
        if (archiveName.empty()) {
            return nullptr;
        }
        auto mesh = std::make_shared<ModelMesh>(meshFromArchive(archiveName));

        // Multi-part objects (PlantA -> PlantA00/01/02, Grapyon -> Body/Head)
        // ship as several archives with no single <Name>.arc, so every
        // companion part is stacked onto the same slot. Which parts exist is
        // probed against the user's own ObjectData listing, never guessed.
        const std::string primaryLower = whitehole::util::toLower(archiveName);
        bool merged = false;
        for (const auto& part : variantArchivesFor(objectName)) {
            if (whitehole::util::toLower(part) == primaryLower) {
                continue; // the primary archive is not its own companion
            }
            ModelMesh partMesh = meshFromArchive(part);
            if (partMesh.empty()) {
                continue;
            }
            // appendModelMesh remaps material/texture indices past dst's tables
            // and concatenates the tables, so a PlantA01 triangle still points
            // at ITS material after PlantA00's are appended.
            appendModelMesh(*mesh, partMesh);
            merged = true;
        }

        if (mesh->empty()) {
            return nullptr;
        }
        if (merged) {
            recomputeMeshBounds(*mesh);
        }
        return mesh;
    } catch (...) {
        // A broken archive must never take the editor down; the object just
        // keeps its placeholder shape.
        return nullptr;
    }
}

ModelProbe ModelLibrary::probe(std::string_view objectName) const {
    ModelProbe report;
    report.objectName = std::string(objectName);
    try {
        report.archiveName = archiveNameFor(objectName);
        report.archiveFound = !report.archiveName.empty();
        if (!report.archiveFound) {
            report.error = "no ObjectData archive matches this object name";
            return report;
        }
        report.partArchives = variantArchivesFor(objectName);
        const auto bytes = filesystem_->read("/ObjectData/" + report.archiveName);
        io::RarcArchive archive(bytes);

        const std::string stem = report.archiveName.substr(0, report.archiveName.size() - 4);
        const auto* entry = findModelEntry(archive, stem);
        if (entry == nullptr) {
            report.error = "archive " + report.archiveName + " holds no usable BMD/BDL";
            return report;
        }
        report.modelFound = true;
        report.modelPath = entry->path;
        report.modelBytes = entry->size;

        const auto modelBytes = archive.read(*entry);
        const auto parsed = smg::parseBmd(modelBytes);
        report.parsed = true;
        report.sceneNodes = parsed.sceneGraph.size();
        report.batches = parsed.batches.size();
        for (const auto& batch : parsed.batches) {
            report.packets += batch.packets.size();
        }

        auto mesh = buildModelMesh(parsed);
        report.triangles = mesh.triangles.size();
        report.skippedPrimitives = mesh.skippedPrimitives;
        report.droppedEmptyMatrixTable = mesh.droppedEmptyMatrixTable;
        report.droppedBadMatrixIndex = mesh.droppedBadMatrixIndex;
        if (!report.usable()) {
            report.error = "parsed but produced 0 triangles";
        }
    } catch (const std::exception& error) {
        report.error = error.what();
    } catch (...) {
        report.error = "unknown failure";
    }
    return report;
}

} // namespace whitehole::render