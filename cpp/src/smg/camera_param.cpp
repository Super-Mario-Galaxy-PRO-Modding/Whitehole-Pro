// CameraParam.bcam data layer -- see camera_param.hpp for the format notes.
//
// Reference data (field order, types, engine defaults, camera classes, known
// event/id names) is transcribed from LaunchCamPlus (SuperHackio) and
// Luma's Workshop's Cameras page -- the two community authorities on SMG
// camera data -- as factual documentation of the game's formats.

#include "whitehole/smg/camera_param.hpp"

#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/hash.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

namespace whitehole::smg {
namespace {

// Variant constructors for default values (BcsvValue = variant<int32_t,
// string, float, int16_t, int8_t>).
BcsvValue intDef(std::int32_t value) { return value; }
BcsvValue floatDef(float value) { return value; }
BcsvValue strDef() { return std::string(); }

// The 52 BCAM fields in LaunchCamPlus' PreferredHashOrder, which is also the
// column order the vanilla-template CameraParam files use. Each entry:
//   name | type | default | SMG2-default override | scope | group | label | tooltip
std::vector<CameraFieldSpec> buildFieldSpecs() {
    using S = CameraFieldScope;
    using G = CameraFieldGroup;
    std::vector<CameraFieldSpec> specs;
    specs.reserve(52);

    // ---- identity (always required columns) ----
    specs.push_back({"version", BcsvType::integer, intDef(196630), intDef(196631),
                     S::general, G::identity, "Version",
                     "Camera engine version: 196630 = SMG1, 196631 = SMG2."});
    specs.push_back({"id", BcsvType::stringOffset, strDef(), std::nullopt,
                     S::general, G::identity, "ID",
                     "Unique camera id. Its prefix (c:/s:/e:/g:/o:) decides which "
                     "context discovers this camera."});
    specs.push_back({"camtype", BcsvType::stringOffset, strDef(), std::nullopt,
                     S::general, G::identity, "Camera type",
                     "Class that implements the camera's behaviour (see the camera type list)."});

    // ---- framing: where the camera sits and how it is oriented ----
    specs.push_back({"string", BcsvType::stringOffset, strDef(), std::nullopt,
                     S::general, G::framing, "Target name",
                     "Object or path name used by some camera types (matrix/vector register cameras)."});
    specs.push_back({"angleB", BcsvType::floatingPoint, floatDef(0.3F), std::nullopt,
                     S::general, G::framing, "Angle B",
                     "Camera-type specific angle in radians: horizontal angle for "
                     "CAM_TYPE_XZ_PARA, rotation dead-zone for CAM_TYPE_TOWER."});
    specs.push_back({"angleA", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Angle A",
                     "Camera-type specific angle in radians: vertical (pitch) angle for "
                     "parallel and tower cameras."});
    specs.push_back({"roll", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Roll",
                     "Rotates the camera around its view axis (180 flips it upside down)."});
    specs.push_back({"dist", BcsvType::floatingPoint, floatDef(1200.0F), std::nullopt,
                     S::general, G::framing, "Distance",
                     "Standoff distance from the target in game units."});
    specs.push_back({"fovy", BcsvType::floatingPoint, floatDef(45.0F), std::nullopt,
                     S::general, G::framing, "Field of view",
                     "Vertical field of view in degrees; keep it between 0 and 179.9."});
    specs.push_back({"num1", BcsvType::integer, intDef(0), intDef(1),
                     S::general, G::framing, "Num 1",
                     "Camera-type specific integer: non-zero enables reset/rounding on "
                     "CAM_TYPE_XZ_PARA; SMG2 defaults to 1."});
    specs.push_back({"num2", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::framing, "Num 2",
                     "Camera-type specific integer used by rail, demo and death cameras."});
    specs.push_back({"loffset", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Forward offset",
                     "Offset along the target's forward vector: higher moves the camera "
                     "in front of the target, lower behind it."});
    specs.push_back({"loffsetv", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Vertical offset",
                     "Offset along the target's up vector: higher moves the camera above "
                     "the target, lower below it."});
    specs.push_back({"woffset.X", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "World offset X",
                     "Constant offset from the target relative to the zone."});
    specs.push_back({"woffset.Y", BcsvType::floatingPoint, floatDef(100.0F), floatDef(0.0F),
                     S::general, G::framing, "World offset Y",
                     "Constant offset from the target relative to the zone "
                     "(SMG1 defaults to 100, SMG2 to 0)."});
    specs.push_back({"woffset.Z", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "World offset Z",
                     "Constant offset from the target relative to the zone."});
    specs.push_back({"wpoint.X", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "World point X",
                     "Fixed world-space point the camera looks at or rotates around."});
    specs.push_back({"wpoint.Y", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "World point Y",
                     "Fixed world-space point the camera looks at or rotates around."});
    specs.push_back({"wpoint.Z", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "World point Z",
                     "Fixed world-space point the camera looks at or rotates around."});
    specs.push_back({"axis.X", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Axis X",
                     "Camera-type specific axis: rotation axis for tower-style cameras, "
                     "player-relative direction otherwise."});
    specs.push_back({"axis.Y", BcsvType::floatingPoint, floatDef(1.0F), std::nullopt,
                     S::general, G::framing, "Axis Y",
                     "Camera-type specific axis: rotation axis for tower-style cameras, "
                     "player-relative direction otherwise."});
    specs.push_back({"axis.Z", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Axis Z",
                     "Camera-type specific axis: rotation axis for tower-style cameras, "
                     "player-relative direction otherwise."});
    specs.push_back({"vpanaxis.X", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Vertical pan axis X",
                     "Axis the Camera Height Arrangement adjusts along (when enabled)."});
    specs.push_back({"vpanaxis.Y", BcsvType::floatingPoint, floatDef(1.0F), std::nullopt,
                     S::general, G::framing, "Vertical pan axis Y",
                     "Axis the Camera Height Arrangement adjusts along (when enabled)."});
    specs.push_back({"vpanaxis.Z", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Vertical pan axis Z",
                     "Axis the Camera Height Arrangement adjusts along (when enabled)."});
    specs.push_back({"up.X", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Up X",
                     "Up vector used to orient the camera."});
    specs.push_back({"up.Y", BcsvType::floatingPoint, floatDef(0.0F), floatDef(1.0F),
                     S::general, G::framing, "Up Y",
                     "Up vector used to orient the camera (SMG2 defaults to +Y, SMG1 to 0)."});
    specs.push_back({"up.Z", BcsvType::floatingPoint, floatDef(0.0F), std::nullopt,
                     S::general, G::framing, "Up Z",
                     "Up vector used to orient the camera."});

    // ---- behaviour: timing, screen bounds, event control ----
    specs.push_back({"camint", BcsvType::integer, intDef(120), std::nullopt,
                     S::general, G::behavior, "Blend-in (frames)",
                     "Frames it takes the camera to move into its active position."});
    specs.push_back({"camendint", BcsvType::integer, intDef(120), std::nullopt,
                     S::event, G::behavior, "Blend-out (frames)",
                     "Frames this event camera takes to deactivate (e: cameras only)."});
    specs.push_back({"gndint", BcsvType::integer, intDef(160), std::nullopt,
                     S::general, G::behavior, "Ground intercept",
                     "Appears unused by the game; kept for compatibility."});
    specs.push_back({"uplay", BcsvType::floatingPoint, floatDef(300.0F), std::nullopt,
                     S::general, G::behavior, "Rise start (Y)",
                     "How high the target must go before the camera starts moving up."});
    specs.push_back({"lplay", BcsvType::floatingPoint, floatDef(800.0F), std::nullopt,
                     S::general, G::behavior, "Sink start (Y)",
                     "How low the target must go before the camera starts moving down."});
    specs.push_back({"pushdelay", BcsvType::integer, intDef(120), std::nullopt,
                     S::general, G::behavior, "Upper bound delay (frames)",
                     "Activation time in frames for the upper screen bound."});
    specs.push_back({"pushdelaylow", BcsvType::integer, intDef(120), std::nullopt,
                     S::general, G::behavior, "Lower bound delay (frames)",
                     "Activation time in frames for the lower screen bound."});
    specs.push_back({"udown", BcsvType::integer, intDef(120), std::nullopt,
                     S::general, G::behavior, "Udown",
                     "Appears unused by the game; kept for compatibility."});
    specs.push_back({"upper", BcsvType::floatingPoint, floatDef(0.3F), std::nullopt,
                     S::general, G::behavior, "Upper screen bound",
                     "Upper screen bound relative to the centre of the screen."});
    specs.push_back({"lower", BcsvType::floatingPoint, floatDef(0.1F), std::nullopt,
                     S::general, G::behavior, "Lower screen bound",
                     "Lower screen bound relative to the centre of the screen."});
    specs.push_back({"evfrm", BcsvType::integer, intDef(0), std::nullopt,
                     S::event, G::behavior, "Event length (frames)",
                     "How many frames this event camera stays active (e: cameras only)."});
    specs.push_back({"evpriority", BcsvType::integer, intDef(1), std::nullopt,
                     S::event, G::behavior, "Event priority",
                     "Event camera priority; the game always uses 1."});
    specs.push_back({"vpanuse", BcsvType::integer, intDef(1), std::nullopt,
                     S::general, G::behavior, "Enable vertical pan",
                     "Enables the Camera Height Arrangement."});

    // ---- flags ----
    specs.push_back({"flag.noreset", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::flags, "No reset",
                     "Disables the camera reset performed when this camera activates."});
    specs.push_back({"flag.nofovy", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::flags, "Enable field of view",
                     "When set, this camera's field of view takes effect."});
    specs.push_back({"flag.lofserpoff", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::flags, "Snap offsets",
                     "loffset/loffsetv snap into place instead of interpolating."});
    specs.push_back({"flag.antibluroff", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::flags, "Disable anti-blur",
                     "Disables the smoothing applied while the camera rotates."});
    specs.push_back({"flag.collisionoff", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::flags, "Ignore collision",
                     "Camera moves through map geometry instead of colliding with it."});
    specs.push_back({"flag.subjectiveoff", BcsvType::integer, intDef(0), std::nullopt,
                     S::general, G::flags, "Disable first person",
                     "Disables the first-person camera for this camera."});
    specs.push_back({"gflag.enableEndErpFrame", BcsvType::integer, intDef(0), std::nullopt,
                     S::game, G::flags, "Group: use end blend",
                     "Deactivating this camera uses gflag.camendint instead of the new "
                     "camera's blend time (c:/g:/s: cameras only)."});
    specs.push_back({"gflag.thru", BcsvType::integer, intDef(0), std::nullopt,
                     S::game, G::flags, "Group: through",
                     "Group transition flag for c:/g:/s: cameras."});
    specs.push_back({"gflag.camendint", BcsvType::integer, intDef(120), std::nullopt,
                     S::game, G::flags, "Group: end blend (frames)",
                     "Deactivation time in frames for this camera (c:/g:/s: cameras only)."});
    specs.push_back({"eflag.enableEndErpFrame", BcsvType::integer, intDef(0), std::nullopt,
                     S::event, G::flags, "Event: use end blend",
                     "Event camera deactivation blends over its own end time (e: cameras only)."});
    specs.push_back({"eflag.enableErpFrame", BcsvType::integer, intDef(0), std::nullopt,
                     S::event, G::flags, "Event: enable blend",
                     "Event camera activation blends in over camint frames (e: cameras only)."});
    return specs;
}

// Spec list + hash index, built once.
const std::vector<CameraFieldSpec>& fieldSpecStore() {
    static const std::vector<CameraFieldSpec> specs = buildFieldSpecs();
    return specs;
}

const std::unordered_map<std::uint32_t, std::size_t>& fieldHashIndex() {
    static const std::unordered_map<std::uint32_t, std::size_t> index = [] {
        std::unordered_map<std::uint32_t, std::size_t> built;
        const auto& specs = fieldSpecStore();
        for (std::size_t i = 0; i < specs.size(); ++i) {
            built.emplace(jmapHash(specs[i].name), i);
        }
        return built;
    }();
    return index;
}

// Cell setters chosen from a spec's declared type.
void writeCell(BcsvTable& table, std::size_t row, const CameraFieldSpec& spec,
               const BcsvValue& value) {
    BcsvRow& cell = table.rows()[row];
    switch (spec.type) {
    case BcsvType::floatingPoint: {
        float text = 0.0F;
        if (const auto* storedFloat = std::get_if<float>(&value)) {
            text = *storedFloat;
        } else if (const auto* storedInt = std::get_if<std::int32_t>(&value)) {
            text = static_cast<float>(*storedInt);
        }
        table.setFloat(cell, spec.name, text);
        break;
    }
    case BcsvType::stringOffset:
    case BcsvType::fixedString: {
        std::string text;
        if (const auto* stored = std::get_if<std::string>(&value)) {
            text = *stored;
        }
        table.setString(cell, spec.name, std::move(text));
        break;
    }
    default: {
        std::int32_t number = 0;
        if (const auto* storedInt = std::get_if<std::int32_t>(&value)) {
            number = *storedInt;
        } else if (const auto* storedFloat = std::get_if<float>(&value)) {
            number = static_cast<std::int32_t>(*storedFloat);
        }
        table.setInt(cell, spec.name, number);
        break;
    }
    }
}

} // namespace

const std::vector<CameraFieldSpec>& cameraFieldSpecs() {
    return fieldSpecStore();
}

const CameraFieldSpec* cameraFieldSpec(std::string_view name) noexcept {
    const auto& specs = fieldSpecStore();
    for (const auto& spec : specs) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

const CameraFieldSpec* cameraFieldSpecForHash(std::uint32_t hash) noexcept {
    const auto& index = fieldHashIndex();
    const auto found = index.find(hash);
    if (found == index.end()) {
        return nullptr;
    }
    return &fieldSpecStore()[found->second];
}

const BcsvValue& cameraFieldDefault(const CameraFieldSpec& spec, std::uint32_t engineVersion) noexcept {
    // SMG2's engine (196631+) flips three defaults (num1, woffset.Y, up.Y);
    // older engines -- including the 196617-era tables -- share the base set.
    if (engineVersion >= kCameraVersionSmg2 && spec.smg2Default.has_value()) {
        return *spec.smg2Default;
    }
    return spec.baseDefault;
}

bool cameraFieldApplies(CameraFieldScope scope, CameraContext context) noexcept {
    switch (scope) {
    case CameraFieldScope::general:
        return true;
    case CameraFieldScope::game:
        // The wiki lists gflag.* as exclusive to c:/g:/s: cameras.
        return context == CameraContext::cube || context == CameraContext::group ||
               context == CameraContext::spawn;
    case CameraFieldScope::event:
        return context == CameraContext::event;
    }
    return false;
}

namespace {

// Every CAM_TYPE_* the two games know, plus the five compatibility aliases.
// Japanese names, availability (SMG1/SMG2) and descriptions follow Luma's
// Workshop's Camera Classes table; labels are short editor-facing English.
std::vector<CameraTypeInfo> buildCameraTypes() {
    std::vector<CameraTypeInfo> types;
    types.reserve(53);
    // id, jpName, label, description, aliasOf, aliasMinVersion, smg1, smg2

    // ---- aliases (resolved only at/after their required engine version) ----
    types.push_back({"CAM_TYPE_BEHIND_DEBUG", "", "Behind (debug)",
                     "Debug alias for the slider camera.", "CAM_TYPE_SLIDER", 196614, true, true});
    types.push_back({"CAM_TYPE_DONKETSU_TEST", "", "Donketsu (test)",
                     "Test alias for a class removed from the shipped game.",
                     "CAM_TYPE_BOSS_DONKETSU", 196612, false, false});
    types.push_back({"CAM_TYPE_EYE_FIXED_THERE_TEST", "", "Eye-fixed there (test)",
                     "Test alias for CAM_TYPE_EYEPOS_FIX_THERE.", "CAM_TYPE_EYEPOS_FIX_THERE",
                     196614, true, true});
    types.push_back({"CAM_TYPE_ICECUBE_PLANET", "", "Ice Cube Planet",
                     "Test alias for CAM_TYPE_CUBE_PLANET.", "CAM_TYPE_CUBE_PLANET", 196617,
                     true, true});
    types.push_back({"CAM_TYPE_INWARD_TOWER_TEST", "", "Inward tower (test)",
                     "Test alias for CAM_TYPE_INWARD_TOWER.", "CAM_TYPE_INWARD_TOWER", 196614,
                     true, true});

    // ---- classes ----
    types.push_back({"CAM_TYPE_2D_SLIDE", "２Ｄスライド", "2D Slide",
                     "Slides along the four screen directions only (no forward/backward).",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_ANIM", "アニメ", "Animated",
                     "Keyframed camera animation; the game creates one dynamically for "
                     "galaxy intros, so vanilla BCAM files do not contain it.",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_BLACK_HOLE", "ブラックホール", "Black hole death",
                     "Death camera used inside black holes (created dynamically).",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_BOSS_DONKETSU", "", "Boss Donketsu",
                     "Class removed from the shipped game; only the DONKETSU_TEST alias "
                     "still references it.", "", 0, false, false});
    types.push_back({"CAM_TYPE_CHARMED_FIX", "サンボ", "Charmed fix",
                     "Fixed camera with a world point, axis and up vector.", "", 0, true, true});
    types.push_back({"CAM_TYPE_CHARMED_VECREG", "ベクトルレジスタ注目", "Charmed vec-reg",
                     "Camera whose angles come from a vector register (uses Target name).",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_CHARMED_VECREG_TOWER", "VecReg角度補正塔カメラ",
                     "Charmed vec-reg tower",
                     "Tower camera whose angles come from a vector register.", "", 0, true, true});
    types.push_back({"CAM_TYPE_CUBE_PLANET", "キューブ惑星", "Cube planet",
                     "Follow variant that rotates along with Mario's current gravitational "
                     "frame.", "", 0, true, true});
    types.push_back({"CAM_TYPE_DEAD", "通常死亡", "Death",
                     "Normal death camera (created dynamically by the game).", "", 0, true, true});
    types.push_back({"CAM_TYPE_DPD", "ＤＰＤ", "DPD",
                     "Pointer-style camera; not used by vanilla stages.", "", 0, true, true});
    types.push_back({"CAM_TYPE_EYEPOS_FIX", "定点", "Eye fixed",
                     "Fixed camera position that looks at Mario.", "", 0, true, true});
    types.push_back({"CAM_TYPE_EYEPOS_FIX_THERE", "その場定点", "Eye fixed (here)",
                     "Stops where the previous camera was and looks at Mario; used when "
                     "flying stars land.", "", 0, true, true});
    types.push_back({"CAM_TYPE_FOLLOW", "フォロー", "Follow",
                     "Basic follow camera orbiting the player at fixed angles and distance.",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_FOO_FIGHTER", "フーファイター", "Foo Fighter",
                     "Flying Power-Up follow camera (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_FOO_FIGHTER_PLANET", "フーファイタープラネット",
                     "Foo Fighter planet",
                     "Planet-gravity variant of the Flying Power-Up camera (SMG1).",
                     "", 0, true, false});
    types.push_back({"CAM_TYPE_FREEZE", "その場完全固定", "Freeze",
                     "Freezes the camera exactly where the previous one left off (SMG2).",
                     "", 0, false, true});
    types.push_back({"CAM_TYPE_FRONT_AND_BACK", "表裏カメラ", "Front and back",
                     "Camera that can swing between the front and back of the target.",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_GROUND", "地面", "Ground",
                     "Camera that hugs the ground plane (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_INNER_CYLINDER", "円筒内部", "Inner cylinder",
                     "Camera constrained inside a cylinder around the target (SMG1).",
                     "", 0, true, false});
    types.push_back({"CAM_TYPE_INWARD_SPHERE", "球内部", "Inward sphere",
                     "Camera constrained inside a sphere around the target (SMG1).",
                     "", 0, true, false});
    types.push_back({"CAM_TYPE_INWARD_TOWER", "塔内部", "Inward tower",
                     "Tower camera that orbits a fixpoint from the inside (SMG1).",
                     "", 0, true, false});
    types.push_back({"CAM_TYPE_MEDIAN_PLANET", "中点注目プラネット", "Median planet",
                     "Planet camera focused on the midpoint between two points.", "", 0, true, true});
    types.push_back({"CAM_TYPE_MEDIAN_TOWER", "中点塔カメラ", "Median tower",
                     "Tower camera focused on the midpoint between two points.", "", 0, true, true});
    types.push_back({"CAM_TYPE_MTXREG_PARALLEL", "マトリクスレジスタ並行", "Matrix-register parallel",
                     "Parallel camera whose orientation comes from a matrix register "
                     "(uses Target name).", "", 0, true, true});
    types.push_back({"CAM_TYPE_OBJ_PARALLEL", "オブジェ並行", "Object parallel",
                     "Parallel camera locked to a specific object (often used while flying).",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_POINT_FIX", "完全固定", "Point fix",
                     "Focused at a set position and angle; does not move or rotate.",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_RACE_FOLLOW", "レース用フォロー", "Race follow",
                     "Follow camera tuned for races, offset by a world point.", "", 0, true, true});
    types.push_back({"CAM_TYPE_RAIL_DEMO", "レールデモ", "Rail demo",
                     "Rail demo camera; not used by vanilla stages.", "", 0, true, true});
    types.push_back({"CAM_TYPE_RAIL_FOLLOW", "レールフォロー", "Rail follow",
                     "Follows along a rail (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_RAIL_WATCH", "レール注目", "Rail watch",
                     "Watches a rail the player is riding.", "", 0, true, true});
    types.push_back({"CAM_TYPE_SLIDER", "スライダー", "Slider",
                     "Slides behind the target; vanilla stages reach it through the "
                     "BEHIND_DEBUG alias.", "", 0, true, true});
    types.push_back({"CAM_TYPE_SPHERE_TRUNDLE", "球トランドル", "Sphere trundle",
                     "Trundles around a sphere (SMG2).", "", 0, false, true});
    types.push_back({"CAM_TYPE_SPIRAL_DEMO", "螺旋デモ", "Spiral demo",
                     "Spiral demo camera; num1 is split into two 16-bit values.", "", 0, true, true});
    types.push_back({"CAM_TYPE_SUBJECTIVE", "主観", "Subjective (first person)",
                     "First-person camera (created dynamically).", "", 0, true, true});
    types.push_back({"CAM_TYPE_TALK", "会話", "Talk",
                     "NPC conversation camera (created dynamically by dialogue).", "", 0, true, true});
    types.push_back({"CAM_TYPE_TOWER", "塔", "Tower",
                     "Focused at a set coordinate and rotates around it based on Mario's "
                     "position.", "", 0, true, true});
    types.push_back({"CAM_TYPE_TOWER_POS", "塔（サブターゲット付き）", "Tower (sub-target)",
                     "Tower camera with a sub-target expressed through wpoint and up.",
                     "", 0, true, true});
    types.push_back({"CAM_TYPE_TRIPOD_PLANET", "三脚惑星", "Tripod planet",
                     "Three-legged planet camera around a world point.", "", 0, true, true});
    types.push_back({"CAM_TYPE_TRIPOD_BOSS", "三脚ボス", "Tripod boss",
                     "Tripod boss camera (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_TRIPOD_BOSS_JOINT", "三脚ボスジョイント", "Tripod boss (joint)",
                     "Tripod boss joint camera (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_CHARMED_TRIPOD_BOSS", "三脚ボスジョイント注視",
                     "Charmed tripod boss",
                     "Tripod boss observation camera (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_TRUNDLE", "トランドル", "Trundle",
                     "Trundles around a fixpoint along an axis.", "", 0, true, true});
    types.push_back({"CAM_TYPE_TWISTED_PASSAGE", "ねじれ回廊", "Twisted passage",
                     "Camera for twisting corridors (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_WATER_FOLLOW", "水中フォロー", "Water follow",
                     "Follow camera used while swimming.", "", 0, true, true});
    types.push_back({"CAM_TYPE_WATER_PLANET", "水中プラネット", "Water planet",
                     "Planet camera used while swimming.", "", 0, true, true});
    types.push_back({"CAM_TYPE_WATER_PLANET_BOSS", "水中プラネットボス", "Water planet boss",
                     "Water planet camera for bosses (SMG1).", "", 0, true, false});
    types.push_back({"CAM_TYPE_WONDER_PLANET", "プラネット", "Wonder planet",
                     "Planet camera driven by angleA and the axis vector.", "", 0, true, true});
    types.push_back({"CAM_TYPE_XZ_PARA", "並行", "Parallel (XZ)",
                     "The most basic camera: moves parallel to Mario's position using "
                     "fixed angles and distance.", "", 0, true, true});
    return types;
}

} // namespace

const std::vector<CameraTypeInfo>& cameraTypes() {
    static const std::vector<CameraTypeInfo> types = buildCameraTypes();
    return types;
}

const CameraTypeInfo* cameraTypeInfo(std::string_view camtype) noexcept {
    for (const auto& type : cameraTypes()) {
        if (type.id == camtype) {
            return &type;
        }
    }
    return nullptr;
}

std::string_view resolveCameraType(std::string_view camtype, std::uint32_t engineVersion) noexcept {
    const CameraTypeInfo* info = cameraTypeInfo(camtype);
    if (info == nullptr || info->aliasOf.empty()) {
        return camtype;
    }
    // The game only honours an alias at/after its required engine version;
    // below that the name stays unresolved (and unknown to the engine).
    if (engineVersion < info->aliasMinVersion) {
        return camtype;
    }
    return info->aliasOf;
}

namespace {

// ---- known id knowledge base ----------------------------------------------
// Japanese event/id names -> English display names, transcribed from
// LaunchCamPlus' EventData tables (which mirror Luma's Workshop's Camera ID
// Names documentation). needsId: the game appends a 3-digit camera-set id;
// needsSub: the id carries a ":XX番目" sub-index as well.
struct KnownEvent {
    std::string_view jp;
    std::string_view en;
    bool needsId;
    bool needsSub;
};

const KnownEvent kKnownEvents[] = {
    {"シナリオスターター", "Scenario Starter", true, true},
    {"スーパースピンドライバー固有出現イベント用", "Launch Star Appearance", true, false},
    {"スーパースピンドライバー", "Launch Star", true, true},
    {"スピンドライバ固有出現イベント用", "Sling Star Appearance", true, false},
    {"スピンドライバ", "Sling Star", true, true},
    {"グリーンスーパースピンドライバー固有出現イベント用", "Green Launch Star Appearance", true, false},
    {"グリーンスーパースピンドライバー", "Green Launch Star", true, true},
    {"ピンクスーパースピンドライバー固有出現イベント用", "Pink Launch Star Appearance", true, false},
    {"ピンクスーパースピンドライバー", "Pink Launch Star", true, true},
    {"Gキャプチャーターゲット固有", "Pull Star Appearance", true, false},
    {"グランドスター固有", "Grand Star Appearance", true, false},
    {"パワースター固有", "Power Star Appearance", true, false},
    {"グリーンスター固有", "Green Star Appearance", true, false},
    {"パワースター出現ポイント固有", "Power Star Appear Point", true, false},
    {"チコ集め固有集めデモカメラ", "Silver Star Completion", true, false},
    {"簡易デモ実行固有簡易デモ", "Simple Demo Executor", true, false},
    {"鍵スイッチ固有", "Key Switch Appearance", true, false},
    {"カプセルケージ固有", "Capsule Cage Opening", true, false},
    {"ゴロ岩カバー檻固有", "Metal Capsule Cage Opening", true, false},
    {"土管固有出現", "Warp Pipe", true, false},
    {"土管（水中用）固有出現", "Warp Pipe (In Water)", true, false},
    {"ウォータープレッシャー固有", "Bubble Shooter", true, false},
    {"ポール固有", "Pole", true, false},
    {"ポール（２方向）固有", "Pole 2 Way", true, false},
    {"ポール（モデル無し）固有", "Pole (No Model)", true, false},
    {"ポール（鉄骨）固有", "Square Pole", true, false},
    {"ポール(モデル無し鉄骨)固有", "Square Pole (No Model)", true, false},
    {"ポール（木Ａ）固有", "Pole Tree A", true, false},
    {"ポール（木B）固有", "Pole Tree B", true, false},
    {"移動用砲台固有", "Player Cannon", true, false},
    {"１ＵＰキノコ固有", "1-UP Appearance", true, false},
    {"ライフアップキノコ固有", "Life Mushroom Appearance", true, false},
    {"？コイン固有", "?-Coin Collection", true, false},
    {"無敵スター固有", "Rainbow Star Appearance", true, false},
    {"伸び植物固有出現デモ", "Sproutle Vine Appearance", true, false},
    {"伸び植物固有掴まり", "Sproutle Vine", true, false},
    {"つるスライダー固有滑り", "Sprauto Vine", true, false},
    {"つる花固有掴まり", "Creeper Plant", true, false},
    {"空中ブランコ固有", "Trapeze Vine", true, false},
    {"スイングロープ固有", "Swinging Vine", true, false},
    {"宇宙まゆ固有ウェイト", "Sling Pod (Waiting)", true, false},
    {"宇宙まゆ固有狙い中", "Sling Pod (Aiming)", true, false},
    {"宇宙まゆ固有攻撃中", "Sling Pod (Flying)", true, false},
    {"ポイハナ固有", "Cataquack Launch", true, false},
    {"音符の妖精固有", "Note Fairy Appearance", true, false},
    {"インフェルノジェネレータ固有出現デモカメラ", "Cosmic Clones Appearance", true, false},
    {"ブラックホール固有", "Black Hole Death", true, false},
    {"ブラックホール[キューブ指定]固有", "Black Hole (Cube) Death", true, false},
    {"ジャンプビーマー", "Spring Beamer", true, true},
    {"ジャンプガーダー", "Guard Beamer", true, true},
    {"バネベーゴマン", "Spring Topman", true, true},
    {"隠れバネベーゴマン", "Hiding Spring Topman", true, true},
    {"モンテ固有", "Chuckster Pianta", true, false},
    {"アイテムドリル固有", "Spin Drill Usage", true, false},
    {"グライバード固有死亡", "Fluzzard Death", true, false},
    {"ゴーストマリオ固有", "Cosmic Race (unknown usage)", true, false},
    {"ゴーストマリオ固有レース開始1", "Cosmic Race Appearance", true, false},
    {"ゴーストマリオ固有レース開始2", "Cosmic Race Staredown", true, false},
    {"ゴーストマリオ固有レース開始3", "Cosmic Race Countdown", true, false},
    {"ゴーストマリオ固有レース終了", "Cosmic Race Losing Camera", true, false},
    {"ハラペココインチコ固有飛行", "[SMG2] Coin Hungry Luma Flight", true, false},
    {"ハラペコスターピースチコ固有変身", "[SMG2] Starbit Hungry Luma Transformation", true, false},
    {"ハラペコスターピースチコ固有飛行", "[SMG2] Starbit Hungry Luma Flight", true, false},
    {"チューブスライダー固有滑り", "Tube Slider", true, false},
    {"チューブスライダー固有飛び出し", "Tube Slider Exit", true, false},
    {"プチポーター固有基点でワープイン", "Minigame Teleporter (Prepare to Warp)", true, false},
    {"プチポーター固有ワープ点でワープアウト", "Minigame Teleporter (Arrive at Destination)", true, false},
    {"プチポーター固有ワープ点でワープイン", "Minigame Teleporter (Prepare return warp)", true, false},
    {"プチポーター固有基点でワープアウト", "Minigame Teleporter (Arrive from returning)", true, false},
    {"ワープカメラ (GroupID)-(The ASCII character ObjArg0+65 represents)", "Warp Pod Template", false, false},
    {"看板固有会話", "Message: Signboard", true, false},
    {"でか看板固有会話", "Message: Big Signboard", true, false},
    {"ピーチ固有会話", "Message: Peach", true, false},
    {"キノピオ固有会話", "Message: Toad", true, false},
    {"郵便屋さんキノピオ固有注目会話", "Message: Mailtoad", true, false},
    {"銀行屋さんキノピオ固有注目会話", "Message: Banktoad", true, false},
    {"ウサギ固有会話", "Message: Star Bunny", true, false},
    {"ロゼッタ固有会話", "Message: Rosalina", true, false},
    {"マイスター固有会話", "Message: Lubba", true, false},
    {"チコ固有会話", "Message: Luma", true, false},
    {"でかチコ固有会話", "Message: Big Luma", true, false},
    {"よろず屋チコ固有独自会話", "Message: Luma Shop", true, false},
    {"ハニークイーン固有会話", "Message: Queen Bee", true, false},
    {"ハニービー固有会話", "Message: Honeybee", true, false},
    {"ペンギン仙人固有会話", "Message: Penguin Elder", true, false},
    {"ペンギンコーチ固有会話", "Message: Penguin Coach", true, false},
    {"ペンギン固有会話", "Message: Penguin", true, false},
    {"パマタリアン固有会話", "Message: Gearmo", true, false},
    {"パマタリアンハンター固有会話", "Message: Gearmo Hunter", true, false},
    {"赤ボム兵固有会話", "Message: Bob-omb Buddy", true, false},
    {"モンテ固有会話", "Message: Pianta", true, false},
    {"ピーチャン固有会話", "Message: Jibberjay", true, false},
    {"モック固有会話", "Message: Whittle", true, false},
    {"さすらいの遊び人(通常会話)固有会話", "Message: The Chimp (NPC)", true, false},
    {"引き戻し", "Pull Back Area", false, false},
    {"水上フォロー", "Water Follow", false, false},
    {"水中フォロー", "Underwater Follow", false, false},
    {"水中プラネット", "Underwater Planet", false, false},
    {"フーファイターカメラ", "Foo Fighter Camera", false, false},
    {"DemoName[CameraPartName]", "Demo Camera Template", false, false},
    {"g:ObjectName:CameraID:0", "Collision Camera Template", false, false},
};

// o: cameras -- names the game looks up after the "o:" prefix.
struct KnownOther {
    std::string_view jp;
    std::string_view en;
};

const KnownOther kKnownOthers[] = {
    {"デフォルトカメラ", "Default Camera"},
    {"デフォルト水中カメラ", "Default Underwater"},
    {"デフォルト水面カメラ", "Default Water Surface"},
    {"デフォルトフーファイターカメラ", "Default Flying Mario"},
    {"スタートカメラ", "Default Spawn Point"},
    {"ズームカメラ", "Zoom Camera (first person)"},
};

// Cameras the game creates at runtime (never stored in a BCAM file). They
// still appear in ids occasionally, so the editor can explain them.
const KnownOther kGameCreatedEvents[] = {
    {"スタートアニメカメラ", "Galaxy Intro Camera"},
    {"ブラックホール", "Default Black Hole Death"},
    {"共通会話カメラ", "Default NPC Dialogue"},
    {"主観カメラ", "First Person Camera"},
    {"昇天カメラ", "Ground Death Camera"},
    {"奈落カメラ", "Air Death Camera"},
    {"変身初出カメラ", "First Time Powerup Get"},
};

// ---- small string helpers --------------------------------------------------
bool isAsciiDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

std::string_view stripTrailingDigits(std::string_view text) noexcept {
    while (!text.empty() && isAsciiDigit(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string stripAllDigits(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (const char ch : text) {
        if (!isAsciiDigit(ch)) {
            stripped.push_back(ch);
        }
    }
    return stripped;
}

// Parses the hex body of a c:/s: id ("000f" -> 15). Tolerates uppercase and
// any digit count; returns -1 when there is no hex digit at all.
std::int32_t parseHexNumber(std::string_view text) noexcept {
    std::uint32_t accum = 0;
    bool any = false;
    int digits = 0;
    for (const char ch : text) {
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<std::uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<std::uint32_t>(ch - 'a') + 10U;
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<std::uint32_t>(ch - 'A') + 10U;
        } else {
            break;
        }
        any = true;
        if (++digits > 8) {
            break;
        }
        accum = (accum << 4U) | digit;
    }
    if (!any) {
        return -1;
    }
    if (accum > 0x7FFFFFFFU) {
        return 0x7FFFFFFF;
    }
    return static_cast<std::int32_t>(accum);
}

std::vector<std::string_view> splitColons(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t colon = text.find(':', start);
        if (colon == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, colon - start));
        start = colon + 1;
    }
    return parts;
}

// Looks a Japanese event name up in kKnownEvents, trying the exact name,
// the name minus its trailing digit run, and the name with all digits
// removed (the "パワースター固有005" family concatenates the set id).
const KnownEvent* findKnownEvent(std::string_view name) {
    for (const auto& entry : kKnownEvents) {
        if (entry.jp == name) {
            return &entry;
        }
    }
    const std::string_view trimmed = stripTrailingDigits(name);
    if (trimmed != name) {
        for (const auto& entry : kKnownEvents) {
            if (entry.jp == trimmed) {
                return &entry;
            }
        }
    }
    const std::string stripped = stripAllDigits(name);
    if (stripped != name) {
        for (const auto& entry : kKnownEvents) {
            if (entry.jp == stripped) {
                return &entry;
            }
        }
    }
    return nullptr;
}

const KnownOther* findKnownOther(std::string_view name) {
    for (const auto& entry : kKnownOthers) {
        if (entry.jp == name) {
            return &entry;
        }
    }
    return nullptr;
}

const KnownOther* findGameCreatedEvent(std::string_view name) {
    for (const auto& entry : kGameCreatedEvents) {
        if (entry.jp == name) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

CameraId parseCameraId(std::string_view id) {
    CameraId result;
    result.raw = std::string(id);
    if (id.size() < 2 || id[1] != ':') {
        return result; // no recognised prefix -> invalid
    }
    const char prefix = id[0];
    const std::string_view rest = id.substr(2);
    switch (prefix) {
    case 'c':
        result.context = CameraContext::cube;
        result.number = parseHexNumber(rest);
        break;
    case 's':
        result.context = CameraContext::spawn;
        result.number = parseHexNumber(rest);
        break;
    case 'e':
        result.context = CameraContext::event;
        result.name = std::string(rest);
        break;
    case 'g':
        result.context = CameraContext::group;
        result.name = std::string(rest);
        break;
    case 'o':
        result.context = CameraContext::other;
        result.name = std::string(rest);
        break;
    default:
        break; // unknown prefix stays invalid
    }
    return result;
}

std::string formatCameraId(const CameraId& id) {
    char buffer[32];
    switch (id.context) {
    case CameraContext::cube:
    case CameraContext::spawn: {
        if (id.number < 0) {
            return id.raw.empty() ? std::string(id.context == CameraContext::cube ? "c:" : "s:")
                                  : id.raw;
        }
        const char* pattern = id.context == CameraContext::cube ? "c:%04x" : "s:%04x";
        std::snprintf(buffer, sizeof(buffer), pattern, static_cast<unsigned int>(id.number));
        return buffer;
    }
    case CameraContext::event:
        return "e:" + id.name;
    case CameraContext::group:
        return "g:" + id.name;
    case CameraContext::other:
        return "o:" + id.name;
    case CameraContext::invalid:
        break;
    }
    return id.raw;
}

const char* cameraContextLabel(CameraContext context) noexcept {
    switch (context) {
    case CameraContext::cube:
        return "Camera area";
    case CameraContext::spawn:
        return "Spawn point";
    case CameraContext::event:
        return "Event";
    case CameraContext::group:
        return "Group";
    case CameraContext::other:
        return "Other";
    case CameraContext::invalid:
        break;
    }
    return "Invalid";
}

std::string cubeCameraIdForArg(std::int32_t objArg0) {
    CameraId id;
    id.context = CameraContext::cube;
    id.number = objArg0;
    return objArg0 < 0 ? std::string() : formatCameraId(id);
}

std::string spawnCameraIdFor(std::int32_t cameraSetId) {
    CameraId id;
    id.context = CameraContext::spawn;
    id.number = cameraSetId;
    return cameraSetId < 0 ? std::string() : formatCameraId(id);
}

namespace {

// Display name for the text after an "e:" prefix.
std::string describeEventId(std::string_view name) {
    // Warp pods use "<event> <digit>-<letter>" (the game's "\s\d-\S" pattern),
    // e.g. "ワープカメラ 1-A" -> group 1, Obj_arg0 0.
    {
        const std::size_t dash = name.find('-');
        if (dash != std::string_view::npos && dash >= 2 && dash + 1 < name.size() &&
            isAsciiDigit(name[dash - 1]) &&
            (name[dash - 2] == ' ' || name[dash - 2] == '\t')) {
            std::size_t runStart = dash;
            while (runStart > 0 && isAsciiDigit(name[runStart - 1])) {
                --runStart;
            }
            const std::string group(name.substr(runStart, dash - runStart));
            const int arg0 = static_cast<int>(name[dash + 1]) - static_cast<int>('A');
            return "Event: Warp Pod (Group " + group + ", Arg0 = " + std::to_string(arg0) + ")";
        }
    }

    const std::vector<std::string_view> parts = splitColons(name);
    if (!parts.empty() && !parts[0].empty()) {
        const KnownEvent* known = findKnownEvent(parts[0]);
        if (known != nullptr) {
            // The camera-set id is either the second colon segment or a digit
            // run concatenated onto the event name ("パワースター固有005").
            std::string setId;
            if (parts.size() >= 2) {
                setId = std::string(parts[1]);
            } else {
                setId = std::string(parts[0].substr(stripTrailingDigits(parts[0]).size()));
            }
            if (known->needsId && !setId.empty()) {
                std::string label(known->en);
                label += ' ';
                label += setId;
                if (known->needsSub && parts.size() >= 3) {
                    std::string_view index = parts[2];
                    if (index.size() >= 6 && index.ends_with("番目")) {
                        index.remove_suffix(6); // UTF-8 "番目"
                    }
                    label += " camera ";
                    label += index;
                }
                return label;
            }
            return std::string(known->en);
        }
    }

    // Demo-scene cameras: "e:DemoName[CameraPart]".
    const std::size_t open = name.find('[');
    const std::size_t close = name.find(']');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
        return "Demo: " + std::string(name);
    }
    return "Event " + std::string(name);
}

} // namespace

std::string describeCameraId(const CameraId& id) {
    switch (id.context) {
    case CameraContext::cube:
        if (id.number >= 0) {
            return "Camera Area " + std::to_string(id.number);
        }
        return id.raw;
    case CameraContext::spawn:
        if (id.number >= 0) {
            return "Spawn Point " + std::to_string(id.number);
        }
        return id.raw;
    case CameraContext::event:
        return describeEventId(id.name);
    case CameraContext::group:
        return "Group: " + id.name;
    case CameraContext::other:
        if (const KnownOther* known = findKnownOther(id.name)) {
            return std::string(known->en);
        }
        if (const KnownOther* created = findGameCreatedEvent(id.name)) {
            return std::string(created->en) + " (created by the game)";
        }
        return id.raw;
    case CameraContext::invalid:
        break;
    }
    return id.raw.empty() ? std::string("Invalid ID") : id.raw;
}

// ---- the table -------------------------------------------------------------
CameraParamTable::CameraParamTable(std::vector<std::uint8_t> bytes, io::Endian endian,
                                   std::uint32_t defaultVersion)
    : table_(std::move(bytes), endian), defaultVersion_(defaultVersion) {}

CameraParamTable CameraParamTable::create(std::uint32_t defaultVersion) {
    CameraParamTable result;
    result.defaultVersion_ = defaultVersion;
    result.ensureRequiredColumns();
    return result;
}

void CameraParamTable::ensureRequiredColumns() {
    (void)table_.ensureField("version", BcsvType::integer);
    (void)table_.ensureField("id", BcsvType::stringOffset);
    (void)table_.ensureField("camtype", BcsvType::stringOffset);
}

void CameraParamTable::fillColumnWithEngineDefaults(const CameraFieldSpec& spec) {
    (void)table_.ensureField(spec.name, spec.type);
    for (std::size_t row = 0; row < table_.rows().size(); ++row) {
        writeCell(table_, row, spec, cameraFieldDefault(spec, engineVersion(row)));
    }
}

BcsvValue CameraParamTable::defaultFor(std::size_t row, const CameraFieldSpec& spec) const {
    return cameraFieldDefault(spec, engineVersion(row));
}

std::size_t CameraParamTable::size() const noexcept {
    return table_.rows().size();
}

std::uint32_t CameraParamTable::engineVersion(std::size_t row) const {
    if (row >= table_.rows().size() || !table_.hasField("version")) {
        return defaultVersion_;
    }
    const std::int32_t stored = table_.getInt(table_.rows()[row], "version",
                                              static_cast<std::int32_t>(defaultVersion_));
    if (stored <= 0) {
        return defaultVersion_;
    }
    return static_cast<std::uint32_t>(stored);
}

std::vector<CameraParamTable::Camera> CameraParamTable::cameras() const {
    std::vector<Camera> list;
    list.reserve(table_.rows().size());
    for (std::size_t row = 0; row < table_.rows().size(); ++row) {
        Camera camera;
        camera.row = row;
        camera.id = getString(row, "id");
        camera.camtype = getString(row, "camtype");
        camera.version = engineVersion(row);
        camera.context = parseCameraId(camera.id).context;
        list.push_back(std::move(camera));
    }
    return list;
}

bool CameraParamTable::hasColumn(std::string_view field) const noexcept {
    return table_.hasField(field);
}

std::optional<BcsvValue> CameraParamTable::storedValue(std::size_t row,
                                                       std::string_view field) const {
    if (row >= table_.rows().size()) {
        return std::nullopt;
    }
    const BcsvValue* value = table_.rawValue(table_.rows()[row], field);
    if (value == nullptr) {
        return std::nullopt;
    }
    return *value;
}

float CameraParamTable::getFloat(std::size_t row, std::string_view field) const {
    const CameraFieldSpec* spec = cameraFieldSpec(field);
    if (const auto stored = storedValue(row, field)) {
        if (const auto* value = std::get_if<float>(&*stored)) {
            return *value;
        }
        if (const auto* value = std::get_if<std::int32_t>(&*stored)) {
            return static_cast<float>(*value);
        }
    }
    if (spec != nullptr) {
        const BcsvValue& fallback = cameraFieldDefault(*spec, engineVersion(row));
        if (const auto* value = std::get_if<float>(&fallback)) {
            return *value;
        }
        if (const auto* value = std::get_if<std::int32_t>(&fallback)) {
            return static_cast<float>(*value);
        }
    }
    return 0.0F;
}

std::int32_t CameraParamTable::getInt(std::size_t row, std::string_view field) const {
    const CameraFieldSpec* spec = cameraFieldSpec(field);
    if (const auto stored = storedValue(row, field)) {
        if (const auto* value = std::get_if<std::int32_t>(&*stored)) {
            return *value;
        }
        if (const auto* value = std::get_if<float>(&*stored)) {
            return static_cast<std::int32_t>(*value);
        }
    }
    if (spec != nullptr) {
        const BcsvValue& fallback = cameraFieldDefault(*spec, engineVersion(row));
        if (const auto* value = std::get_if<std::int32_t>(&fallback)) {
            return *value;
        }
        if (const auto* value = std::get_if<float>(&fallback)) {
            return static_cast<std::int32_t>(*value);
        }
    }
    return 0;
}

std::string CameraParamTable::getString(std::size_t row, std::string_view field) const {
    const CameraFieldSpec* spec = cameraFieldSpec(field);
    if (const auto stored = storedValue(row, field)) {
        if (const auto* value = std::get_if<std::string>(&*stored)) {
            return *value;
        }
    }
    if (spec != nullptr) {
        const BcsvValue& fallback = cameraFieldDefault(*spec, engineVersion(row));
        if (const auto* value = std::get_if<std::string>(&fallback)) {
            return *value;
        }
    }
    return std::string();
}

void CameraParamTable::setFloat(std::size_t row, std::string_view field, float value) {
    if (row >= table_.rows().size()) {
        throw std::out_of_range("camera row is out of range");
    }
    if (!table_.hasField(field)) {
        const CameraFieldSpec* spec = cameraFieldSpec(field);
        if (spec == nullptr) {
            throw std::runtime_error("Unknown camera field '" + std::string(field) + "'");
        }
        fillColumnWithEngineDefaults(*spec);
    }
    table_.setFloat(table_.rows()[row], field, value);
}

void CameraParamTable::setInt(std::size_t row, std::string_view field, std::int32_t value) {
    if (row >= table_.rows().size()) {
        throw std::out_of_range("camera row is out of range");
    }
    if (!table_.hasField(field)) {
        const CameraFieldSpec* spec = cameraFieldSpec(field);
        if (spec == nullptr) {
            throw std::runtime_error("Unknown camera field '" + std::string(field) + "'");
        }
        fillColumnWithEngineDefaults(*spec);
    }
    table_.setInt(table_.rows()[row], field, value);
}

void CameraParamTable::setString(std::size_t row, std::string_view field, std::string value) {
    if (row >= table_.rows().size()) {
        throw std::out_of_range("camera row is out of range");
    }
    if (!table_.hasField(field)) {
        const CameraFieldSpec* spec = cameraFieldSpec(field);
        if (spec == nullptr) {
            throw std::runtime_error("Unknown camera field '" + std::string(field) + "'");
        }
        fillColumnWithEngineDefaults(*spec);
    }
    table_.setString(table_.rows()[row], field, std::move(value));
}

std::size_t CameraParamTable::addCamera(std::string_view id, std::string_view camtype,
                                        std::uint32_t version) {
    ensureRequiredColumns();
    const std::size_t row = table_.addRow();
    // Every parameter column the table carries is filled with the engine
    // default for this version: addRow() zero-fills cells, but a zero-filled
    // `dist` column would read as distance 0 instead of the engine's 1200.
    for (const auto& spec : cameraFieldSpecs()) {
        if (!table_.hasField(spec.name)) {
            continue;
        }
        if (spec.name == "version" || spec.name == "id" || spec.name == "camtype") {
            continue;
        }
        writeCell(table_, row, spec, cameraFieldDefault(spec, version));
    }
    table_.setInt(table_.rows()[row], "version", static_cast<std::int32_t>(version));
    table_.setString(table_.rows()[row], "id", std::string(id));
    table_.setString(table_.rows()[row], "camtype", std::string(camtype));
    return row;
}

bool CameraParamTable::removeCamera(std::size_t row) {
    return table_.removeRow(row);
}

// ---- in-game pose ------------------------------------------------------------
namespace {

// The game's spherical cameras share one eye placement: the eye sits on a
// sphere around the look point, driven by two radian angles. The decompiled
// sources pin the convention down:
//  * CamTranslatorParallel stores angleB verbatim (radians) and angleA as
//    degrees in the engine while the file holds radians -- the engine works
//    in degrees on top of radian data, so both agree on the same rotation;
//  * CamTranslatorFix places the camera with polar(dist, axis.x, axis.y) and
//    then negates the offset (the camera stands opposite its direction
//    vector);
//  * the decompiled Follow class carries _54 = 10deg, _58 = 20deg at dist
//    1200, the same spherical family.
// The LaunchCamPlus per-type panels edit the angles in degrees but store
// radians; the eye sits at
//   eye = look + dist * (cos(angleA) * cos(angleB), sin(angleA),
//                        cos(angleA) * sin(angleB))
// so angleB=0 is +X and both zeros sit at dist along +X looking back along -X.
constexpr float kRadiansPerDegree = 3.141592653589793F / 180.0F;

math::Vec3f sphericalEye(const math::Vec3f& look, float angleA, float angleB, float dist) noexcept {
    const float cosA = std::cos(angleA);
    return {look.x + dist * cosA * std::cos(angleB), look.y + dist * std::sin(angleA),
            look.z + dist * cosA * std::sin(angleB)};
}

math::Vec3f finiteUp(math::Vec3f up) noexcept {
    if (up.length() < 0.000001F) {
        return {0.0F, 1.0F, 0.0F};
    }
    return up.normalized();
}

// A number that is actually a number, or `fallback`. Solver inputs arrive from a
// file a modder can hand-edit, and one NaN poisons every matrix the viewport
// builds from the pose -- which looks exactly like a broken editor. The solver's
// contract is that it never fails, so a non-finite input is pulled back to the
// documented default instead of being passed through.
[[nodiscard]] float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] math::Vec3f finiteOr(math::Vec3f value, math::Vec3f fallback) noexcept {
    return {finiteOr(value.x, fallback.x), finiteOr(value.y, fallback.y),
            finiteOr(value.z, fallback.z)};
}

// The fallbacks are CameraPreviewParams' own defaults (camera_param.hpp), so a
// corrupt row previews as the same camera a missing row would.
[[nodiscard]] CameraPreviewParams sanitisePreviewParams(const CameraPreviewParams& params) {
    CameraPreviewParams safe = params;
    safe.wpoint = finiteOr(safe.wpoint, math::Vec3f{});
    safe.axis = finiteOr(safe.axis, math::Vec3f{0.0F, 1.0F, 0.0F});
    safe.up = finiteOr(safe.up, math::Vec3f{});
    safe.woffset = finiteOr(safe.woffset, math::Vec3f{});
    safe.angleA = finiteOr(safe.angleA, 0.0F);
    safe.angleB = finiteOr(safe.angleB, 0.3F);
    safe.dist = finiteOr(safe.dist, 1200.0F);
    safe.roll = finiteOr(safe.roll, 0.0F);
    safe.fovy = finiteOr(safe.fovy, 45.0F);
    safe.loffset = finiteOr(safe.loffset, 0.0F);
    safe.loffsetv = finiteOr(safe.loffsetv, 0.0F);
    return safe;
}

// Solver dispatch: which claim the preview makes about a camtype.
PoseSupport supportForType(std::string_view camtype) noexcept {
    if (camtype == "CAM_TYPE_XZ_PARA" || camtype == "CAM_TYPE_POINT_FIX" ||
        camtype == "CAM_TYPE_EYEPOS_FIX") {
        return PoseSupport::exact;
    }
    if (camtype == "CAM_TYPE_FOLLOW" || camtype == "CAM_TYPE_OBJ_PARALLEL" ||
        camtype == "CAM_TYPE_MTXREG_PARALLEL" || camtype == "CAM_TYPE_CUBE_PLANET" ||
        camtype == "CAM_TYPE_WONDER_PLANET" || camtype == "CAM_TYPE_TOWER" ||
        camtype == "CAM_TYPE_TOWER_POS" || camtype == "CAM_TYPE_WATER_FOLLOW" ||
        camtype == "CAM_TYPE_WATER_PLANET" || camtype == "CAM_TYPE_TRUNDLE" ||
        camtype == "CAM_TYPE_2D_SLIDE" || camtype == "CAM_TYPE_MEDIAN_PLANET" ||
        camtype == "CAM_TYPE_MEDIAN_TOWER" || camtype == "CAM_TYPE_RACE_FOLLOW" ||
        camtype == "CAM_TYPE_RAIL_WATCH" || camtype == "CAM_TYPE_FRONT_AND_BACK" ||
        camtype == "CAM_TYPE_GROUND" || camtype == "CAM_TYPE_FOO_FIGHTER" ||
        camtype == "CAM_TYPE_FOO_FIGHTER_PLANET" || camtype == "CAM_TYPE_CHARMED_FIX" ||
        camtype == "CAM_TYPE_CHARMED_VECREG" || camtype == "CAM_TYPE_CHARMED_VECREG_TOWER" ||
        camtype == "CAM_TYPE_TRIPOD_PLANET" || camtype == "CAM_TYPE_TWISTED_PASSAGE") {
        return PoseSupport::spherical;
    }
    return PoseSupport::none;
}

} // namespace

CameraPreviewParams cameraPreviewParams(const CameraParamTable& table, std::size_t row) {
    CameraPreviewParams params;
    params.id = table.getString(row, "id");
    params.camtype = table.getString(row, "camtype");
    params.version = table.engineVersion(row);
    params.wpoint = {table.getFloat(row, "wpoint.X"), table.getFloat(row, "wpoint.Y"),
                     table.getFloat(row, "wpoint.Z")};
    params.axis = {table.getFloat(row, "axis.X"), table.getFloat(row, "axis.Y"),
                   table.getFloat(row, "axis.Z")};
    params.up = {table.getFloat(row, "up.X"), table.getFloat(row, "up.Y"),
                 table.getFloat(row, "up.Z")};
    params.woffset = {table.getFloat(row, "woffset.X"), table.getFloat(row, "woffset.Y"),
                      table.getFloat(row, "woffset.Z")};
    params.angleA = table.getFloat(row, "angleA");
    params.angleB = table.getFloat(row, "angleB");
    params.dist = table.getFloat(row, "dist");
    params.roll = table.getFloat(row, "roll");
    params.fovy = table.getFloat(row, "fovy");
    params.loffset = table.getFloat(row, "loffset");
    params.loffsetv = table.getFloat(row, "loffsetv");
    return params;
}

PoseSupport cameraPoseSupport(std::string_view camtype, std::uint32_t engineVersion) noexcept {
    return supportForType(resolveCameraType(camtype, engineVersion));
}

GameCameraPose solveGameCameraPose(const CameraPreviewParams& raw, const math::Vec3f& target,
                                   PoseSupport* support) noexcept {
    // This is file data, so every number is sanitised before it is used:
    // `params` and `tracked` below are the safe copies (see
    // sanitisePreviewParams), and the solve itself never reports a failure.
    const CameraPreviewParams params = sanitisePreviewParams(raw);
    const math::Vec3f tracked = finiteOr(target, math::Vec3f{});
    const std::string_view resolved = resolveCameraType(params.camtype, params.version);
    const PoseSupport claimed = supportForType(resolved);
    if (support != nullptr) {
        *support = claimed;
    }

    GameCameraPose pose;
    pose.fovYRadians = params.fovy * kRadiansPerDegree;
    pose.rollRadians = params.roll;

    // The pivot the eye swings around: the tracked target plus the constant
    // zone-space offset (woffset).
    const math::Vec3f pivot{tracked.x + params.woffset.x, tracked.y + params.woffset.y,
                            tracked.z + params.woffset.z};

    if (resolved == "CAM_TYPE_EYEPOS_FIX") {
        // FixedPoint: the eye is pinned to wpoint and the camera watches the
        // tracked target (CamTranslatorFixedPoint hands wpoint to the class).
        pose.eye = params.wpoint;
        pose.at = pivot;
        pose.up = finiteUp(params.up);
        return pose;
    }
    if (resolved == "CAM_TYPE_POINT_FIX") {
        // Fix: polar(dist, axis.x, axis.y) around wpoint, negated, aimed at
        // the target. axis holds ROTATION DEGREES here (the LCP PointFix panel
        // edits them as degrees), so convert first.
        const float axisX = params.axis.x * kRadiansPerDegree;
        const float axisY = params.axis.y * kRadiansPerDegree;
        const math::Vec3f dir = sphericalEye({0.0F, 0.0F, 0.0F}, axisY, axisX, 1.0F);
        pose.eye = {params.wpoint.x - dir.x * params.dist, params.wpoint.y - dir.y * params.dist,
                    params.wpoint.z - dir.z * params.dist};
        pose.at = pivot;
        pose.up = finiteUp(params.up);
        return pose;
    }
    if (resolved == "CAM_TYPE_XZ_PARA") {
        // Parallel: spherical eye around the pivot, plus the tracked push
        // (loffset along the ground-plane view-forward, loffsetv up).
        pose.eye = sphericalEye(pivot, params.angleA, params.angleB, params.dist);
        const math::Vec3f ground{pivot.x - pose.eye.x, 0.0F, pivot.z - pose.eye.z};
        const float groundLen = ground.length();
        if (groundLen > 0.000001F) {
            pose.eye.x += ground.x / groundLen * params.loffset;
            pose.eye.z += ground.z / groundLen * params.loffset;
        }
        pose.eye.y += params.loffsetv;
        pose.at = {pivot.x, pivot.y + params.loffsetv, pivot.z};
        pose.up = finiteUp(params.up);
        return pose;
    }
    if (claimed == PoseSupport::spherical) {
        // Spherical orbit family (follow/tower/object/matrix-register/...):
        // file-radian angles on the shared sphere around the pivot.
        pose.eye = sphericalEye(pivot, params.angleA, params.angleB, params.dist);
        pose.at = pivot;
        pose.up = finiteUp(params.up);
        return pose;
    }
    // Dynamic-only (event/subjective/anim/...) or unknown: look at the pivot
    // from dist along +X so every camera still yields a sane frame.
    pose.eye = {pivot.x + params.dist, pivot.y, pivot.z};
    pose.at = pivot;
    pose.up = finiteUp(params.up);
    return pose;
}

} // namespace whitehole::smg














