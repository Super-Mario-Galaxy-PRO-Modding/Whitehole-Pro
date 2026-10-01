#include "whitehole/smg/hash.hpp"
#include "whitehole/smg/scenario_model.hpp"

#include <algorithm>

namespace whitehole::smg {
namespace {

// The scenario table's own columns, in the order StageHelper's
// populateScenarioFieldsScenarioData() writes them. Everything that is NOT one of
// these is a per-zone layer mask, which is the format's one surprising rule.
constexpr std::string_view kScenarioNo = "ScenarioNo";
constexpr std::string_view kScenarioName = "ScenarioName";
constexpr std::string_view kPowerStarId = "PowerStarId";
constexpr std::string_view kAppearPowerStarObj = "AppearPowerStarObj";
constexpr std::string_view kComet = "Comet";
constexpr std::string_view kLuigiModeTimer = "LuigiModeTimer";
// SMG2 only. SMG1 stores the same idea as an integer IsHidden instead.
constexpr std::string_view kPowerStarType = "PowerStarType";
constexpr std::string_view kCometLimitTimer = "CometLimitTimer";
constexpr std::string_view kZoneName = "ZoneName";
constexpr std::string_view kWorldNo = "WorldNo";

// True when a column is one of the scenario table's own fields rather than a
// per-zone layer mask. The hashes are computed with fieldHash() -- the SAME
// function BcsvTable's setString/setInt use -- rather than written down as
// constants. Hardcoding them looked reasonable and was wrong: the values that
// `hash ScenarioNo` prints are jmapHash, and hardcoding jmapHash meant the test
// never matched, so the "copy the layer columns" pass treated ScenarioNo as a
// layer and silently overwrote the copy's id with the source's.
bool isScenarioOwnHash(std::uint32_t hash) noexcept {
    static const std::vector<std::uint32_t> own = {
        fieldHash(kScenarioNo),       fieldHash(kScenarioName),
        fieldHash(kPowerStarId),      fieldHash(kAppearPowerStarObj),
        fieldHash(kComet),            fieldHash(kLuigiModeTimer),
        fieldHash(kPowerStarType),    fieldHash(kCometLimitTimer)};
    for (const std::uint32_t candidate : own) {
        if (hash == candidate) {
            return true;
        }
    }
    return false;
}

} // namespace

// True when a column holds an integer the layer mask could live in. Used before
// writing a mask, so a name that is already taken by, say, a string column is
// refused instead of being silently reinterpreted.
bool isIntegerColumn(const BcsvTable& table, std::string_view name) noexcept {
    const auto index = table.fieldIndex(name);
    if (!index) {
        return false;
    }
    const BcsvType type = table.fields()[*index].type;
    return type == BcsvType::integer || type == BcsvType::integer2;
}

StarType starTypeFromText(std::string_view text) noexcept {
    if (text == "Green") {
        return StarType::green;
    }
    if (text == "Hidden") {
        return StarType::hidden;
    }
    if (text == "Normal") {
        return StarType::normal;
    }
    // Empty (no such column) and anything unrecognised both land here: this build
    // does not know what the game would do with them, and the stored text is kept
    // so writing it back is lossless.
    return StarType::custom;
}

std::string_view starTypeText(StarType type) noexcept {
    switch (type) {
    case StarType::normal:
        return "Normal";
    case StarType::green:
        return "Green";
    case StarType::hidden:
        return "Hidden";
    case StarType::custom:
        break;
    }
    return "Normal";
}

std::string starTypeLabel(std::string_view text) {
    // An EMPTY value is not "Normal": it means this galaxy has no PowerStarType
    // column at all (SMG1 uses an integer IsHidden instead). Saying "not recorded"
    // is honest; saying "Normal star" would be a claim the file does not support.
    if (text.empty()) {
        return "Not recorded";
    }
    switch (starTypeFromText(text)) {
    case StarType::green:
        return "Green star";
    case StarType::hidden:
        return "Hidden (counts as no star)";
    case StarType::normal:
        return "Normal star";
    case StarType::custom:
        break;
    }
    // An unfamiliar value is shown as-is rather than guessed at: this build does
    // not know what the game would do with it, and saying so beats inventing a
    // meaning the game never had.
    return std::string("Custom (\"") + std::string(text) + "\")";
}

bool starTypeIsSpecial(std::string_view text) noexcept {
    // The game's own rule in getNormalPowerStarNum: neither Hidden nor Green
    // counts as an ordinary star.
    return text == "Hidden" || text == "Green";
}

std::vector<std::string> scenarioLayerNames() {
    // 16 layers, LayerA..LayerP. The game's loop is `for (i = 0; i < 16; i++)`
    // over bit i, so this is the whole addressable range.
    std::vector<std::string> layers;
    layers.reserve(16);
    for (int bit = 0; bit < 16; ++bit) {
        std::string name = "Layer";
        name.push_back(static_cast<char>('A' + bit));
        layers.push_back(std::move(name));
    }
    return layers;
}

int scenarioLayerBit(std::string_view layerName) noexcept {
    // "LayerB" -> 1. Anything else, "Common" included, owns no bit.
    constexpr std::string_view prefix = "Layer";
    if (layerName.size() != prefix.size() + 1) {
        return -1;
    }
    if (layerName.compare(0, prefix.size(), prefix) != 0) {
        return -1;
    }
    const char letter = layerName[prefix.size()];
    if (letter < 'A' || letter > 'P') {
        return -1;
    }
    return letter - 'A';
}

bool Scenario::starTypeIsGreen() const noexcept { return powerStarType == "Green"; }
bool Scenario::starTypeIsHidden() const noexcept { return powerStarType == "Hidden"; }

std::string Scenario::starTypeDescription() const { return starTypeLabel(powerStarType); }

// ---- the model --------------------------------------------------------------

ScenarioModel::ScenarioModel(BcsvTable& scenarioData, BcsvTable& zoneList,
                             std::vector<std::string> zones, std::int32_t worldNo)
    : scenarioData_(&scenarioData), zoneList_(&zoneList), zones_(std::move(zones)),
      worldNo_(worldNo) {
    readAll();
}

void ScenarioModel::refresh() { readAll(); }

Scenario ScenarioModel::readScenario(std::size_t row) const {
    Scenario out;
    out.row = row;
    const BcsvRow& cells = scenarioData_->rows()[row];
    out.number = scenarioData_->getInt(cells, kScenarioNo, 0);
    out.name = scenarioData_->getString(cells, kScenarioName);
    out.powerStarId = scenarioData_->getInt(cells, kPowerStarId, 0);
    // PowerStarType is SMG2-only. An SMG1 galaxy has no such column, and the game
    // expresses the same thing as an integer IsHidden; getString on a missing or
    // numeric column yields empty, which leaves the type Unknown rather than
    // inventing a "Normal" the file never said.
    out.powerStarType = scenarioData_->getString(cells, kPowerStarType);
    // The comet marker is the `Comet` COLUMN, not a flag: the template carries
    // "Normal"/"Dark" there, and an empty or "Normal" value means an ordinary
    // mission. The game's own scenarios use both spellings across the two games,
    // so a non-empty value that is neither is the comet case.
    const std::string cometKind = scenarioData_->getString(cells, kComet);
    out.comet = !cometKind.empty() && cometKind != "Normal";
    out.cometName = cometKind;
    out.cometTimer = scenarioData_->getInt(cells, kCometLimitTimer, 0);
    out.worldNo = worldNo_;
    return out;
}

void ScenarioModel::readAll() {
    scenarios_.clear();
    const std::size_t count = scenarioData_->rows().size();
    scenarios_.reserve(count);
    for (std::size_t row = 0; row < count; ++row) {
        scenarios_.push_back(readScenario(row));
    }
}

std::optional<std::size_t> ScenarioModel::findScenario(std::int32_t scenarioNo) const {
    // Read the TABLE, not the cached scenarios_ vector. The cache is only rebuilt
    // by readAll(), and setLayerActive() deliberately does not re-read it (nothing
    // about the scenario list changed), so a scan over the cache goes stale the
    // moment a layer is toggled -- which used to hand back an id that was already
    // taken and silently create two scenarios with the same ScenarioNo.
    const std::size_t count = scenarioData_->rows().size();
    for (std::size_t row = 0; row < count; ++row) {
        if (scenarioData_->getInt(scenarioData_->rows()[row], kScenarioNo, 0) == scenarioNo) {
            return row;
        }
    }
    return std::nullopt;
}

std::size_t ScenarioModel::powerStarCount() const {
    std::size_t count = 0;
    for (const Scenario& scenario : scenarios_) {
        // Exactly the game's getPowerStarNum: any non-zero id counts, whatever the
        // type says. Hidden stars are counted here and filtered out separately.
        if (scenario.powerStarId != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t ScenarioModel::ordinaryPowerStarCount() const {
    std::size_t count = 0;
    for (const Scenario& scenario : scenarios_) {
        if (scenario.powerStarId != 0 && !starTypeIsSpecial(scenario.powerStarType)) {
            ++count;
        }
    }
    return count;
}

std::int32_t ScenarioModel::nextFreeScenarioNumber() const {
    // The game addresses scenarios by ScenarioNo from 1 upward, so start there.
    // findScenario reads the table directly, so this stays correct even when the
    // caller has toggled layers since the last refresh().
    for (std::int32_t candidate = 1; candidate < 0x10000; ++candidate) {
        if (!findScenario(candidate).has_value()) {
            return candidate;
        }
    }
    return 1;
}

// A row that lists every column the game can store, so a brand-new scenario is
// the same shape as an existing one. ensureField() appends without disturbing the
// columns already there, which is what keeps the untouched table byte-exact.
//
// The PowerStarType / CometLimitTimer pair is SMG2-ONLY, and adding it to an
// SMG1 galaxy would put a column in the file the game has never seen -- the same
// mistake the Scenarios panel refuses to make when it offers a star-type picker.
// So they are created only when the table is ALREADY an SMG2 one, judged by
// whether the galaxy carries the SMG1 marker instead. A brand-new table (no
// columns at all) is taken as SMG2, which is the common case and matches
// populateScenarioFieldsScenarioData's own default.
void ensureScenarioColumns(BcsvTable& table) {
    table.ensureField(kScenarioNo, BcsvType::integer);
    table.ensureField(kScenarioName, BcsvType::stringOffset);
    table.ensureField(kPowerStarId, BcsvType::integer);
    // Types exactly as StageHelper declares them: Comet is a string, and the
    // per-zone mask columns are integers. Guessing these wrong makes setInt throw
    // on a string column, which is how the first draft of this file failed.
    table.ensureField(kAppearPowerStarObj, BcsvType::stringOffset);
    table.ensureField(kComet, BcsvType::stringOffset);
    table.ensureField(kLuigiModeTimer, BcsvType::integer);
    // SMG1 stores IsHidden instead of the SMG2 pair. Its presence is the signal.
    const bool smg1 = table.hasField("IsHidden");
    if (!smg1) {
        table.ensureField(kPowerStarType, BcsvType::stringOffset);
        table.ensureField(kCometLimitTimer, BcsvType::integer);
    }
}

std::size_t ScenarioModel::addScenario(std::string_view name,
                                       std::optional<std::size_t> copyFrom) {
    // The id is resolved BEFORE the row exists: nextFreeScenarioNumber() scans the
    // existing scenarios, and a freshly added row carries the default 0, which
    // would otherwise make the scan see it as id 0 and hand back a number that is
    // one too low.
    const std::int32_t freshNumber = nextFreeScenarioNumber();
    ensureScenarioColumns(*scenarioData_);
    const std::size_t row = scenarioData_->addRow();
    // The id always comes from that scan, even when copying: a duplicate
    // ScenarioNo would make the game resolve to the first one, so "add a mission
    // like this one" must never clone the id.
    scenarioData_->setInt(scenarioData_->rows()[row], kScenarioNo, freshNumber);
    scenarioData_->setString(scenarioData_->rows()[row], kScenarioName, std::string(name));
    if (copyFrom.has_value() && *copyFrom < scenarioData_->rows().size() && *copyFrom != row) {
        // The source is re-fetched per use and the row is addressed by index, never
        // through a held reference: ensureField() below can append a column, which
        // rewrites the stored rows and would leave a BcsvRow& dangling mid-write.
        const std::size_t source = *copyFrom;
        scenarioData_->setInt(scenarioData_->rows()[row], kPowerStarId,
                              scenarioData_->getInt(scenarioData_->rows()[source], kPowerStarId, 0));
        scenarioData_->setString(scenarioData_->rows()[row], kPowerStarType,
                                 scenarioData_->getString(scenarioData_->rows()[source], kPowerStarType));
        scenarioData_->setString(scenarioData_->rows()[row], kComet,
                                 scenarioData_->getString(scenarioData_->rows()[source], kComet));
        scenarioData_->setInt(scenarioData_->rows()[row], kCometLimitTimer,
                              scenarioData_->getInt(scenarioData_->rows()[source], kCometLimitTimer, 0));
        // The per-zone layer columns ride along, so the copy opens with the same
        // layers active as the scenario it was copied from. Only INTEGER columns
        // are copied this way: a galaxy may carry extra non-own columns of other
        // types (the bundled template has one), and setIntById on a string column
        // throws rather than quietly doing nothing.
        //
        // The hashes are snapshotted into a local vector FIRST. fields() is the
        // non-const accessor, and writing a cell invalidates its cached index --
        // holding BcsvField& across a write and re-reading .hash afterwards read a
        // stale value, which is how the copied row kept the SOURCE's ScenarioNo.
        std::vector<std::uint32_t> maskColumns;
        for (const BcsvField& field : scenarioData_->fields()) {
            if (!isScenarioOwnHash(field.hash) && field.type == BcsvType::integer) {
                maskColumns.push_back(field.hash);
            }
        }
        for (const std::uint32_t hash : maskColumns) {
            scenarioData_->setIntById(scenarioData_->rows()[row], hash,
                                      scenarioData_->getIntById(scenarioData_->rows()[source],
                                                                 hash, 0));
        }
    } else {
        // A brand-new scenario has no PowerStarType and no comet: both stay empty,
        // which is what "the file says nothing" looks like. Writing a "Normal"
        // here would be inventing a value the game's own template never sets on a
        // fresh row.
        scenarioData_->setString(scenarioData_->rows()[row], kPowerStarType, "");
        scenarioData_->setString(scenarioData_->rows()[row], kComet, "");
    }
    readAll();
    return row;
}

bool ScenarioModel::removeScenario(std::size_t row) {
    if (row >= scenarioData_->rows().size()) {
        return false;
    }
    (void)scenarioData_->removeRow(row);
    readAll();
    return true;
}

void ScenarioModel::renameScenario(std::size_t row, std::string_view name) {
    if (row >= scenarioData_->rows().size()) {
        return;
    }
    ensureScenarioColumns(*scenarioData_);
    scenarioData_->setString(scenarioData_->rows()[row], kScenarioName, std::string(name));
    readAll();
}

void ScenarioModel::setPowerStar(std::size_t row, std::int32_t powerStarId) {
    if (row >= scenarioData_->rows().size()) {
        return;
    }
    ensureScenarioColumns(*scenarioData_);
    scenarioData_->setInt(scenarioData_->rows()[row], kPowerStarId,
                          std::max(powerStarId, 0));
    readAll();
}

void ScenarioModel::setPowerStarType(std::size_t row, std::string_view type) {
    if (row >= scenarioData_->rows().size()) {
        return;
    }
    ensureScenarioColumns(*scenarioData_);
    scenarioData_->setString(scenarioData_->rows()[row], kPowerStarType, std::string(type));
    readAll();
}

void ScenarioModel::setComet(std::size_t row, bool comet, std::int32_t timerFrames) {
    if (row >= scenarioData_->rows().size()) {
        return;
    }
    ensureScenarioColumns(*scenarioData_);
    BcsvRow& cells = scenarioData_->rows()[row];
    // "Dark" is the value the bundled template uses for a comet mission, so that
    // is what turning the flag on writes.
    scenarioData_->setString(cells, kComet, comet ? "Dark" : "Normal");
    scenarioData_->setInt(cells, kCometLimitTimer, comet ? std::max(timerFrames, 0) : 0);
    readAll();
}

bool ScenarioModel::setScenarioNumber(std::size_t row, std::int32_t scenarioNo) {
    if (row >= scenarioData_->rows().size() || scenarioNo < 0) {
        return false;
    }
    // The game resolves a scenario by id, so two rows sharing one would make the
    // first unreachable. Refuse rather than silently shadowing it. findScenario
    // returns a ROW index, which is exactly what `row` is, so the comparison is
    // between two row indices.
    const std::optional<std::size_t> existing = findScenario(scenarioNo);
    if (existing.has_value() && *existing != row) {
        return false;
    }
    ensureScenarioColumns(*scenarioData_);
    scenarioData_->setInt(scenarioData_->rows()[row], kScenarioNo, scenarioNo);
    readAll();
    return true;
}

// ---- the zone x layer matrix -----------------------------------------------

std::int32_t ScenarioModel::layerMask(std::size_t row, std::string_view zoneName) const {
    if (row >= scenarioData_->rows().size()) {
        return 0;
    }
    // A zone with no column of its own means "Common only". That is not an error:
    // a galaxy only grows a zone column once a scenario actually uses a layer
    // there, so the absence is the normal state for most zones.
    if (!scenarioData_->hasField(zoneName)) {
        return 0;
    }
    return scenarioData_->getInt(scenarioData_->rows()[row], zoneName, 0);
}

std::vector<std::string> ScenarioModel::activeLayers(std::size_t row,
                                                    std::string_view zoneName) const {
    // Byte-for-byte the logic of GalaxyArchive.getActiveLayerNames() in the Java
    // source: Common always, then every set bit from LayerA upward.
    std::vector<std::string> layers;
    layers.push_back("Common");
    const std::int32_t mask = layerMask(row, zoneName);
    for (int bit = 0; bit < 16; ++bit) {
        if ((mask & (1 << bit)) != 0) {
            std::string name = "Layer";
            name.push_back(static_cast<char>('A' + bit));
            layers.push_back(std::move(name));
        }
    }
    return layers;
}

bool ScenarioModel::setLayerActive(std::size_t row, std::string_view zoneName,
                                   std::string_view layerName, bool active) {
    if (row >= scenarioData_->rows().size() || zoneName.empty()) {
        return false;
    }
    const int bit = scenarioLayerBit(layerName);
    if (bit < 0) {
        return false; // "Common" owns no bit, and neither does a typo
    }
    // The zone's column is created on first use, typed integer because that is
    // what the game reads. ensureField() appends, so existing columns keep their
    // offsets and every stored value stays where it was.
    if (!scenarioData_->hasField(zoneName)) {
        (void)scenarioData_->ensureField(zoneName, BcsvType::integer);
    } else if (!isIntegerColumn(*scenarioData_, zoneName)) {
        // The name is taken by a column of another type. Overwriting it would
        // corrupt whatever the game keeps there, so the edit is refused.
        return false;
    }
    const std::int32_t mask = scenarioData_->getInt(scenarioData_->rows()[row], zoneName, 0);
    const std::int32_t bitMask = 1 << bit;
    const std::int32_t updated = active ? (mask | bitMask) : (mask & ~bitMask);
    scenarioData_->setInt(scenarioData_->rows()[row], zoneName, updated);
    // No re-read needed: a layer mask is not one of the fields scenarios() copies,
    // so the cached list is still accurate. It DOES need re-reading before any
    // caller indexes scenarios() by ROW, because the cache is positional.
    return true;
}

// ---- the zone list ---------------------------------------------------------

std::optional<std::size_t> ScenarioModel::addZone(std::string_view zoneName) {
    if (zoneName.empty()) {
        return std::nullopt;
    }
    // The game looks zones up by name, so a second row with the same name is a
    // trap, not a harmless duplicate.
    for (const std::string& existing : zones_) {
        if (existing == zoneName) {
            return std::nullopt;
        }
    }
    (void)zoneList_->ensureField(kZoneName, BcsvType::stringOffset);
    const std::size_t row = zoneList_->addRow();
    zoneList_->setString(zoneList_->rows()[row], kZoneName, std::string(zoneName));
    zones_.push_back(std::string(zoneName));
    return row;
}

bool ScenarioModel::removeZone(std::size_t row) {
    if (row >= zoneList_->rows().size() || row >= zones_.size()) {
        return false;
    }
    (void)zoneList_->removeRow(row);
    // The zone's layer column is left in place on purpose. ScenarioData is shared
    // across the galaxy's zones and dropping a column would rewrite every scenario
    // row; an orphaned column costs nothing and the game ignores it.
    zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(row));
    return true;
}

bool ScenarioModel::moveZone(std::size_t from, std::size_t to) {
    if (from >= zones_.size() || to >= zones_.size() || from == to) {
        return false;
    }
    // The whole row moves, not just the name, so any other column the game ever
    // adds to ZoneList travels with it.
    const BcsvRow carried = zoneList_->rows()[from];
    zoneList_->rows().erase(zoneList_->rows().begin() + static_cast<std::ptrdiff_t>(from));
    zoneList_->rows().insert(zoneList_->rows().begin() + static_cast<std::ptrdiff_t>(to), carried);
    const std::string name = zones_[from];
    zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(from));
    zones_.insert(zones_.begin() + static_cast<std::ptrdiff_t>(to), name);
    return true;
}

} // namespace whitehole::smg