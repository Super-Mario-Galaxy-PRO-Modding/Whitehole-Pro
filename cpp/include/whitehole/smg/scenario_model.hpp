#pragma once

// ScenarioData.bcsv / ZoneList.bcsv -- the per-galaxy scenario tables, i.e. what
// each mission ("scenario") of a galaxy declares: its name, which Power Star it
// hands out, that star's type, its comet settings, and which layers of which
// zone are active during it.
//
// Format facts, all confirmed against the GAME's own reader rather than guessed
// (GalaxyTools/Garigari + Petari, src/Game/System/ScenarioDataParser.cpp, and the
// Java parity source src/whitehole/smg/GalaxyArchive.java):
//  * ScenarioData.bcsv has one row per scenario. `ScenarioNo` is the game's
//    1-based scenario id and is what the game looks a scenario up by -- NOT the
//    row order. A scenario with id 0 awards no star: the game skips it when
//    counting stars, which is how a galaxy hides a slot without deleting a row.
//  * `PowerStarId` is the star the scenario awards. 0 means it awards nothing.
//  * `PowerStarType` (SMG2 only) names the star: "Normal", "Green", "Hidden". The
//    game compares it as a string and special-cases Hidden/Green when counting
//    stars. SMG1 has no such column and uses an integer IsHidden instead, so an
//    absent column reads as "unknown" and is never guessed at.
//  * `Comet` is a separate STRING column holding the comet kind ("Dark" for a comet
//    mission, empty otherwise), NOT a flag, and `CometLimitTimer` its length.
//  * EVERY REMAINING COLUMN IS NAMED AFTER A ZONE and holds that zone's LAYER
//    MASK for this scenario: bit 0 = LayerA, bit 1 = LayerB, ... bit 15 = LayerP.
//    "Common" is always active and is never a bit. That is why the template
//    galaxy's single extra column, RedBlueExGalaxy, holds 1/2/4 for the three
//    scenarios that use LayerA/LayerB/LayerC.
//  * The mask is read RAW here. The game's getValueU32() multiplies by two before
//    handing it to its caller, so that multiply belongs to the caller and must not
//    be folded in here, or every bit would land one position off.

#include "whitehole/smg/bcsv.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// How a scenario's awarded star behaves. `custom` keeps any value this build does
// not know, so an unfamiliar game string round-trips untouched instead of being
// normalised into something the game never wrote.
enum class StarType { normal, green, hidden, custom };

// Parses PowerStarType. Anything unrecognised is StarType::custom, and the stored
// text is kept in Scenario::powerStarType, so writing it back is lossless.
[[nodiscard]] StarType starTypeFromText(std::string_view text) noexcept;
[[nodiscard]] std::string_view starTypeText(StarType type) noexcept;
// The English the panel shows for a stored PowerStarType value.
[[nodiscard]] std::string starTypeLabel(std::string_view text);
// True for the types the game treats specially (Hidden/Green), i.e. the
// scenarios excluded from the "normal" star count.
[[nodiscard]] bool starTypeIsSpecial(std::string_view text) noexcept;

// The 16 layers a mask can address, as the game spells them.
[[nodiscard]] std::vector<std::string> scenarioLayerNames();
// Resolves "LayerB" to its bit index (1), or -1 when the name is not a layer.
// "Common" deliberately returns -1: it is always active and owns no bit.
[[nodiscard]] int scenarioLayerBit(std::string_view layerName) noexcept;

// The canonical spelling of a layer name: "common" and "layera" as an SMG1
// archive stores them become "Common" and "LayerA"; anything that is not a layer
// comes back unchanged so a caller can tell "not a layer" from "renamed".
//
// WHY THIS EXISTS, because it is not cosmetic. StageArchive loads a table's layer
// from the directory name AS STORED, and SMG1 archives are all-lowercase -- so an
// SMG1 zone yields "common"/"layera" while an SMG2 zone yields "Common"/"LayerA".
// scenarioLayerBit() above is case-SENSITIVE, so calling it on that field returns
// -1 for EVERY object in EVERY SMG1 zone: a layer filter built on it would work
// perfectly on SMG2 and silently show nothing on SMG1, with no error anywhere.
// That is the same bug family as the loose find() in RARC -- a lookup that is
// right for half the inputs and quietly wrong for the rest.
//
// Normalising once, at load, fixes every consumer at once. The symptom of it
// not being done is already visible: cli.cpp compares layer names with
// equalIgnoreCase because it had to cope, and edit/rails.cpp writes a literal
// "Common" with a capital C beside data read from the archive.
[[nodiscard]] std::string canonicalLayerName(std::string_view layerName);

// One scenario row, resolved. Every field falls back to a documented default
// rather than throwing, so a hand-edited or truncated table still lists.
struct Scenario {
    std::size_t row{0};           // index into the table
    std::int32_t number{0};       // ScenarioNo; 0 means "awards nothing"
    std::string name;             // ScenarioName
    std::int32_t powerStarId{0};  // 0 = awards no star
    // The PowerStarAppear OBJECT this mission's star is born from -- the object
    // name exactly as the game stores it ("PowerStarAppear_Boss_Bowser"), not a
    // friendly label. The game reads it through getAppearPowerStarObjName, so it
    // is what actually decides WHICH star object appears in the level.
    //
    // Empty is a real value, not "unknown": it means the mission has no
    // appearance override and the game falls back to its own default. This was
    // the one column Scenaristar's signature feature uses and the model threw it
    // away on read.
    std::string appearPowerStarObj;
    // SMG1's timed-mode limit in frames. The column is in BOTH games' fixed hash
    // set, but it only MEANS something for a Luigi timed mission, so the panel
    // offers it there rather than showing an inert number on every mission.
    std::int32_t luigiModeTimer{0};
    // The stored PowerStarType, verbatim. Empty means the galaxy has no such
    // column (SMG1), which is NOT the same as "Normal" -- the panel shows it as
    // unknown rather than guessing a value the file never held.
    std::string powerStarType;
    bool comet{false};            // a comet scenario (the `Comet` column)
    std::string cometName;        // that column verbatim ("Normal"/"Dark"/"")
    std::int32_t cometTimer{0};   // frames before it turns into a comet
    std::int32_t worldNo{0};      // GalaxyInfo.WorldNo for the galaxy

    // Does this scenario hand out a star? Exactly the game's own test.
    [[nodiscard]] bool awardsStar() const noexcept { return powerStarId != 0; }
    [[nodiscard]] bool starTypeIsGreen() const noexcept;
    [[nodiscard]] bool starTypeIsHidden() const noexcept;
    // The type as the editor shows it, e.g. "Green star", or an honest "unknown"
    // when the galaxy carries no PowerStarType column at all.
    [[nodiscard]] std::string starTypeDescription() const;
};

// A view over one galaxy's scenario tables. Owns nothing: it edits the tables in
// place, which is what lets the whole thing be undone with the same whole-table
// byte snapshot the camera subsystem already uses.
class ScenarioModel {
public:
    ScenarioModel(BcsvTable& scenarioData, BcsvTable& zoneList, std::vector<std::string> zones,
                  std::int32_t worldNo);

    // ---- scenarios -------------------------------------------------------
    [[nodiscard]] const std::vector<Scenario>& scenarios() const noexcept { return scenarios_; }
    [[nodiscard]] std::size_t scenarioCount() const noexcept { return scenarios_.size(); }
    // Resolves one scenario by its GAME id (ScenarioNo), which is how the game
    // addresses it, returning its ROW index. Nullopt when no row carries that id.
    // Reads the table directly rather than the scenarios() cache, so the answer is
    // right even after an edit that deliberately did not rebuild that cache.
    [[nodiscard]] std::optional<std::size_t> findScenario(std::int32_t scenarioNo) const;
    // Every scenario that awards a star: the count the game's getPowerStarNum
    // computes. ...and ordinaryPowerStarCount() is the count of the ones it does
    // NOT treat as Hidden or Green.
    [[nodiscard]] std::size_t powerStarCount() const;
    [[nodiscard]] std::size_t ordinaryPowerStarCount() const;
    // The lowest unused ScenarioNo, so "add scenario" cannot collide with one.
    [[nodiscard]] std::int32_t nextFreeScenarioNumber() const;

    // Appends a scenario. `copyFrom` duplicates an existing one's settings (for
    // "add mission like this one"); the game id always comes from
    // nextFreeScenarioNumber(). Returns the new row index.
    [[nodiscard]] std::size_t addScenario(std::string_view name,
                                          std::optional<std::size_t> copyFrom = std::nullopt);
    // Removes a scenario row. False when the row does not exist.
    bool removeScenario(std::size_t row);
    void renameScenario(std::size_t row, std::string_view name);
    void setPowerStar(std::size_t row, std::int32_t powerStarId);
    // Clears with an empty string, which is how the game is told to use its own
    // default appearance. Passing a name that is not a known PowerStarAppear*
    // object is still allowed -- the model must not silently reject a star object
    // a modder added.
    void setAppearPowerStarObj(std::size_t row, std::string_view objectName);
    void setLuigiModeTimer(std::size_t row, std::int32_t frames);
    void setPowerStarType(std::size_t row, std::string_view type);
    void setComet(std::size_t row, bool comet, std::int32_t timerFrames);
    // Moves a scenario to a different game id, keeping the rest of the row.
    // False for a bad row, or when the id is already used by a different row.
    bool setScenarioNumber(std::size_t row, std::int32_t scenarioNo);

    // ---- the zone x layer matrix ------------------------------------------
    [[nodiscard]] const std::vector<std::string>& zones() const noexcept { return zones_; }
    // Zones a scenario activates, always starting with "Common". This is
    // getActiveLayerNames() from the Java source: Common, then every LayerA..P
    // the mask names, in order.
    [[nodiscard]] std::vector<std::string> activeLayers(std::size_t row,
                                                        std::string_view zoneName) const;
    // The raw mask for a zone in a scenario. A zone with no column of its own is
    // 0 (Common only), which is how a galaxy predating per-zone layers reads.
    [[nodiscard]] std::int32_t layerMask(std::size_t row, std::string_view zoneName) const;
    // Turns one layer on/off inside a zone's mask, creating the zone's column on
    // first use. "Common" owns no bit, so it is rejected.
    bool setLayerActive(std::size_t row, std::string_view zoneName, std::string_view layerName,
                        bool active);

    // ---- the zone list ----------------------------------------------------
    // Adds a zone row. Returns the new row index; nullopt when the name is
    // already listed, because the game looks zones up by name and a duplicate is
    // a trap rather than a harmless second entry.
    [[nodiscard]] std::optional<std::size_t> addZone(std::string_view zoneName);
    bool removeZone(std::size_t row);
    // Moves a zone row, keeping the rest of its row intact. False on a bad index.
    bool moveZone(std::size_t from, std::size_t to);

    // Re-reads every scenario and the zone list from the tables. Call after any
    // mutation; the panel calls it once per frame rather than per edit, exactly as
    // the camera panel re-validates its row indices.
    void refresh();

private:
    [[nodiscard]] Scenario readScenario(std::size_t row) const;
    void readAll();

    BcsvTable* scenarioData_;
    BcsvTable* zoneList_;
    std::vector<std::string> zones_;
    std::int32_t worldNo_{0};
    std::vector<Scenario> scenarios_;
};

} // namespace whitehole::smg