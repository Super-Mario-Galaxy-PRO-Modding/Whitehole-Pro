#pragma once

// CameraParam.bcam -- the per-zone camera table that holds every camera the
// game can activate: camera areas (c:), spawn points (s:), event/demo-scene
// cameras (e:, including the scenario starter cameras), group cameras (g:)
// and the default/underwater/first-person "other" cameras (o:).
//
// Format facts (see Luma's Workshop "Cameras" and LaunchCamPlus):
//  * A BCSV inside a zone's Map archive at /Stage/camera/CameraParam.bcam.
//  * Every row has three required fields -- version, id, camtype -- plus an
//    optional set of parameter columns. The game only stores a column when at
//    least one camera differs from the engine default, so readers must fall
//    back to the engine defaults for any column the file does not carry.
//  * Each row's `version` is the camera engine version: 196630 (SMG1) or
//    196631 (SMG2); a few defaults (num1, woffset.Y, up.Y) differ per engine.
//  * The id encodes the camera's context (prefix) which decides how the game
//    discovers the camera; the wiki documents c:/s:/e:/g:/o: formats.
//
// Defaults and field order follow LaunchCamPlus (the community's proven
// camera editor). NOTE: the Luma's Workshop wiki's defaults table appears to
// swap angleA/dist (and differs on camendint/gflag.camendint); we follow LCP,
// which matches the vanilla-template CameraParam values.

#include "whitehole/io/binary_file.hpp"
#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/bcsv.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// Camera engine versions stamped into each row's `version` field.
inline constexpr std::uint32_t kCameraVersionSmg1 = 196630;
inline constexpr std::uint32_t kCameraVersionSmg2 = 196631;

// Archive-relative path of a zone's camera table. Lookups in RarcArchive are
// case-insensitive, so this also matches SMG1's all-lowercase archives.
inline constexpr std::string_view kCameraParamPath = "/Stage/camera/CameraParam.bcam";

// Camera engine version the editor stamps on new rows for a game type
// (1 = SMG1, 2 = SMG2); unknown games default to SMG2's engine.
[[nodiscard]] inline std::uint32_t cameraVersionForGame(int gameType) noexcept {
    return gameType == 1 ? kCameraVersionSmg1 : kCameraVersionSmg2;
}

// ---- camera id ------------------------------------------------------------
// The five id contexts documented on Luma's Workshop. parseCameraId() never
// fails: an id with no recognised prefix yields CameraContext::invalid.
enum class CameraContext : std::uint8_t { invalid, cube, spawn, event, group, other };

struct CameraId {
    CameraContext context{CameraContext::invalid};
    // cube/spawn: the numeric camera-set id the game formats as %04x
    // (decimal here, because that is what Obj_arg0 / CameraSetId store).
    // -1 when the id carries no parseable number.
    std::int32_t number{-1};
    // event: the text after "e:"; group/other: the text after "g:"/"o:".
    std::string name;
    // The id exactly as written in the file.
    std::string raw;

    [[nodiscard]] bool operator==(const CameraId& other) const = default;
};

// Splits an id into its context + payload. Tolerates uppercase hex, short
// hex runs and unknown event names (context stays recognised; number -1).
[[nodiscard]] CameraId parseCameraId(std::string_view id);

// Rebuilds a canonical id. cube/spawn reformat through "%04x" (lowercase,
// zero padded, exactly what the game generates); every other context echoes
// its prefix + name, which round-trips the original text verbatim.
[[nodiscard]] std::string formatCameraId(const CameraId& id);

// Human context label for the UI ("Camera area", "Spawn point", ...).
[[nodiscard]] const char* cameraContextLabel(CameraContext context) noexcept;

// The printf the game uses: a camera area's Obj_arg0 of 15 -> "c:000f",
// a spawn point's CameraSetId of 60 -> "s:003c". Hex digits are lowercase.
[[nodiscard]] std::string cubeCameraIdForArg(std::int32_t objArg0);
[[nodiscard]] std::string spawnCameraIdFor(std::int32_t cameraSetId);

// ---- the known-name dictionaries -------------------------------------------
// The Japanese event names the game looks a camera id up by, and the English the
// editor shows instead (the panel's font has no CJK glyphs, so the JP text is
// never displayed -- these tables are how a row becomes readable, and how an
// author picks a name without ever typing Japanese). Transcribed from
// LaunchCamPlus' EventData tables; see camera_param.cpp for the data.
struct KnownCameraEvent {
    std::string_view jp;    // "シナリオスターター" (stored in the file)
    std::string_view en;    // "Scenario Starter" (shown in the editor)
    bool needsId{false};    // the game appends a 3-digit camera-set id
    bool needsSub{false};   // ...and a ":NN番目" sub-index as well
};

// e: event names, in LaunchCamPlus' order.
[[nodiscard]] const std::vector<KnownCameraEvent>& cameraKnownEvents();
// o: names the game looks up after the "o:" prefix.
[[nodiscard]] const std::vector<KnownCameraEvent>& cameraKnownOthers();
// Cameras the game builds at runtime. They never appear in a BCAM file, so the
// editor only names them -- it must never offer one as something to add.
[[nodiscard]] const std::vector<KnownCameraEvent>& cameraGameCreatedEvents();
// Exact lookup by stored (Japanese) name, or nullptr when unknown.
[[nodiscard]] const KnownCameraEvent* cameraKnownEvent(std::string_view jp) noexcept;

// Builds the id the game itself writes for an e: camera:
// ("シナリオスターター", 5, 1) -> "e:シナリオスターター:005:01番目".
// The set id is %03d and the sub-index %02d followed by the 6-byte "番目"
// suffix, which is exactly the form describeEventId() parses back.
[[nodiscard]] std::string eventCameraIdFor(std::string_view jpEvent, int setId,
                                           int subIndex) noexcept;

// Friendly English name for an id: known event names are translated
// ("e:シナリオスターター:005:01番目" -> "Scenario Starter 005 camera 01"),
// c:/s: get "Camera Area 15" / "Spawn Point 60" style labels, g:/o: fall
// back through their dictionaries, and anything unknown echoes the raw id so
// the UI never shows an empty label.
[[nodiscard]] std::string describeCameraId(const CameraId& id);

// ---- field specification ---------------------------------------------------
// Which ids a field applies to (from the wiki's exclusivity column).
enum class CameraFieldScope : std::uint8_t {
    general, // every camera
    game,    // gflag.* -- c:, g:, s: cameras only
    event,   // eflag./evfrm/evpriority/camendint -- e: cameras only
};

// UI grouping so the property panel can lay fields out in sensible sections.
enum class CameraFieldGroup : std::uint8_t { identity, framing, behavior, flags };

struct CameraFieldSpec {
    std::string_view name;   // BCSV field name (hashed with jmapHash)
    BcsvType type;           // BCSV storage type
    BcsvValue baseDefault;   // default for every engine version ...
    std::optional<BcsvValue> smg2Default; // ... unless an SMG2 override exists
    CameraFieldScope scope;
    CameraFieldGroup group;
    std::string_view label;   // friendly UI label
    std::string_view tooltip; // what the field actually does
};

// All 52 known BCAM fields in the game's preferred order (LaunchCamPlus
// PreferredHashOrder), for reading fallbacks, new-column types and the editor.
[[nodiscard]] const std::vector<CameraFieldSpec>& cameraFieldSpecs();

// Spec by exact field name, or nullptr when the name is unknown.
[[nodiscard]] const CameraFieldSpec* cameraFieldSpec(std::string_view name) noexcept;
// Spec by field hash (jmapHash(name)), or nullptr -- this is how the table
// maps an already-loaded BCSV column back onto its spec without name lookup.
[[nodiscard]] const CameraFieldSpec* cameraFieldSpecForHash(std::uint32_t hash) noexcept;

// Engine default for one field at a given camera engine version.
[[nodiscard]] const BcsvValue& cameraFieldDefault(const CameraFieldSpec& spec,
                                                  std::uint32_t engineVersion) noexcept;

// Whether a field makes sense for a camera with the given id context
// (mirrors the wiki's exclusivity column; used to filter the editor UI).
[[nodiscard]] bool cameraFieldApplies(CameraFieldScope scope, CameraContext context) noexcept;

// ---- camera type registry --------------------------------------------------
struct CameraTypeInfo {
    std::string_view id;      // "CAM_TYPE_XZ_PARA"
    std::string_view jpName;  // in-game Japanese class name ("並行")
    std::string_view label;   // short English label for pickers
    std::string_view description;
    std::string_view aliasOf; // empty unless this entry is an alias
    std::uint32_t aliasMinVersion{0}; // aliases only resolve at/after this version
    bool smg1{true};          // class exists in SMG1's engine
    bool smg2{true};          // class exists in SMG2's engine
};

// Every known CAM_TYPE_* (including the five compatibility aliases).
[[nodiscard]] const std::vector<CameraTypeInfo>& cameraTypes();

// Exact registry lookup (aliases are their own entries), or nullptr.
[[nodiscard]] const CameraTypeInfo* cameraTypeInfo(std::string_view camtype) noexcept;

// Resolves an alias to its real class when `engineVersion` allows it (the
// game only honours aliases at/after the alias' required version); non-aliases
// and unknown types are returned unchanged. Unknown ids come back verbatim so
// a modder's custom camtype survives a round trip.
[[nodiscard]] std::string_view resolveCameraType(std::string_view camtype,
                                                 std::uint32_t engineVersion) noexcept;

// ---- in-game pose ------------------------------------------------------------

class CameraParamTable;

// Where the game itself would put the camera for one BCAM entry and one
// target point, for the viewport preview. Angles are BCAM-native radians,
// matching the stored data (LaunchCamPlus' per-type panels display degrees,
// but the file -- and the decompiled translators -- speak radians).

// A solved camera: eye position, look-at point, up vector, vertical field of
// view and roll about the view axis, all in world units and SMG-native
// conventions (Y-up world, metres-like units like the placement data).
struct GameCameraPose {
    math::Vec3f eye{0.0F, 0.0F, 0.0F};
    math::Vec3f at{0.0F, 0.0F, 0.0F};
    math::Vec3f up{0.0F, 1.0F, 0.0F};
    float fovYRadians{0.7853982F}; // 45 degrees, the engine default
    float rollRadians{0.0F};
};

// A previewed camera entry, flattened from one CameraParamTable row:
// resolved camtype plus the handful of parameters the solvers read. Missing
// columns resolve to the row's engine defaults before this is built, so every
// solver sees complete data.
struct CameraPreviewParams {
    std::string id;
    std::string camtype; // unresolved id, exactly as stored
    math::Vec3f wpoint{0.0F, 0.0F, 0.0F};
    math::Vec3f axis{0.0F, 1.0F, 0.0F};
    math::Vec3f up{0.0F, 0.0F, 0.0F};
    math::Vec3f woffset{0.0F, 0.0F, 0.0F};
    float angleA{0.0F};
    float angleB{0.3F};
    float dist{1200.0F};
    float roll{0.0F};
    float fovy{45.0F};
    float loffset{0.0F};
    float loffsetv{0.0F};
    std::uint32_t version{kCameraVersionSmg2};
};

// Fills a CameraPreviewParams from a table row of the named camera, falling
// back to that row's engine defaults for any column the sparse file omits.
[[nodiscard]] CameraPreviewParams cameraPreviewParams(const CameraParamTable& table,
                                                      std::size_t row);

// How faithfully the solver below claims to reproduce a camera type:
// exact means the computation mirrors the decompiled translator plus the
// spherical follow it drives; spherical means the eye sits on the same
// angleA/angleB/dist sphere the game's spherical cameras share, while the
// look target is the documented fixed/task point; none means the type is
// dynamic-only (or unknown) and the pose falls back to the eye it declares.
enum class PoseSupport : std::uint8_t { exact, spherical, none };

// Solver class for a camtype (aliases resolved first): how the preview looks
// through this camera. Unknown ids report none.
[[nodiscard]] PoseSupport cameraPoseSupport(std::string_view camtype,
                                            std::uint32_t engineVersion) noexcept;

// Solves one camera for one target point. `target` is the point the game
// tracks (Mario's position for c:/s:/g:/o: cameras): the eye stays relative
// to it, and point-fix-style types aim at it. Never fails: unsupported types
// and degenerate inputs fall back to looking at the target from dist along
// +X (reporting none), so the preview button is harmless on any entry.
[[nodiscard]] GameCameraPose solveGameCameraPose(const CameraPreviewParams& params,
                                                 const math::Vec3f& target,
                                                 PoseSupport* support = nullptr) noexcept;

// ---- the table -------------------------------------------------------------
// Typed, defaults-aware view over a CameraParam.bcam BCSV. The underlying
// BcsvTable stays in file order, so an untouched load saves back with byte
// identical content (the BCSV writer may append alignment padding) and a
// reader never loses a field it did not understand; parameter columns are only
// created when the editor writes a field the sparse file did not carry.
class CameraParamTable {
public:
    CameraParamTable() = default;
    // `defaultVersion` seeds reads when the file has no version column and
    // stamps new rows; pass cameraVersionForGame(zone game type).
    CameraParamTable(std::vector<std::uint8_t> bytes, io::Endian endian, std::uint32_t defaultVersion);

    // A brand-new minimal table carrying only the three required columns
    // (version, id, camtype) -- exactly what a fresh zone would need.
    [[nodiscard]] static CameraParamTable create(std::uint32_t defaultVersion);

    // One camera row: identity + resolved context, rebuilt on demand.
    struct Camera {
        std::size_t row{0};
        std::string id;
        std::string camtype;
        std::uint32_t version{0};
        CameraContext context{CameraContext::invalid};
    };
    [[nodiscard]] std::vector<Camera> cameras() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Engine version a row runs on (its `version`, or the table default).
    [[nodiscard]] std::uint32_t engineVersion(std::size_t row) const;

    // Typed reads. A column the file does not carry falls back to the engine
    // default for that row's version; unknown fields fall back to a zero value.
    [[nodiscard]] float getFloat(std::size_t row, std::string_view field) const;
    [[nodiscard]] std::int32_t getInt(std::size_t row, std::string_view field) const;
    [[nodiscard]] std::string getString(std::size_t row, std::string_view field) const;
    // True when the column exists (the value came from the file rather than
    // the engine default).
    [[nodiscard]] bool hasColumn(std::string_view field) const noexcept;
    // Raw stored cell, or nullopt when the column is absent.
    [[nodiscard]] std::optional<BcsvValue> storedValue(std::size_t row, std::string_view field) const;

    // Typed writes. Writing a parameter the sparse file lacked creates the
    // column (typed from the spec) and fills every OTHER row with its engine
    // default first, so no existing camera changes meaning.
    void setFloat(std::size_t row, std::string_view field, float value);
    void setInt(std::size_t row, std::string_view field, std::int32_t value);
    void setString(std::size_t row, std::string_view field, std::string value);

    // Appends a camera. All existing columns are filled with the engine
    // defaults for `version`, then id/camtype are applied. Returns the row.
    [[nodiscard]] std::size_t addCamera(std::string_view id, std::string_view camtype,
                                        std::uint32_t version);
    // Removes a camera row. Returns false when the row does not exist.
    bool removeCamera(std::size_t row);

    [[nodiscard]] std::uint32_t defaultVersion() const noexcept { return defaultVersion_; }
    [[nodiscard]] const BcsvTable& table() const noexcept { return table_; }
    [[nodiscard]] BcsvTable& table() noexcept { return table_; }
    [[nodiscard]] std::vector<std::uint8_t> serialize() const { return table_.serialize(); }

private:
    void ensureRequiredColumns();
    // Creates a missing parameter column and fills every row with its engine
    // default, so adding a column never changes what an existing camera means.
    void fillColumnWithEngineDefaults(const CameraFieldSpec& spec);
    [[nodiscard]] BcsvValue defaultFor(std::size_t row, const CameraFieldSpec& spec) const;

    BcsvTable table_;
    std::uint32_t defaultVersion_{kCameraVersionSmg2};
};

// True when some row already stores this id, comparing the PARSED form rather
// than the text: "c:000f" and "c:000F" are the same camera, while a
// differently-prefixed or differently-numbered id is not. An unparseable id
// never matches, so a half-typed id cannot be reported as a duplicate.
[[nodiscard]] bool cameraIdExists(const CameraParamTable& table, std::string_view id) noexcept;

// The id the game would generate for the next free camera of this event: the
// same event + set id with the lowest ":NN番目" sub-index not already stored.
// Scans a bounded range and returns the canonical id for sub-index 0 if a table
// somehow already holds them all.
[[nodiscard]] std::string nextFreeEventCameraId(const CameraParamTable& table,
                                                std::string_view jpEvent,
                                                int setId);

} // namespace whitehole::smg


