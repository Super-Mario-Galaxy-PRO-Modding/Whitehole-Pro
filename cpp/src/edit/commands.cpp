#include "whitehole/edit/commands.hpp"

#include <stdexcept>
#include <utility>

namespace whitehole::edit {
namespace {

smg::BcsvTable& tableAt(smg::StageArchive& stage, std::size_t tableIndex) {
    auto& tables = stage.tables();
    if (tableIndex >= tables.size()) {
        throw std::out_of_range("Stage table index is out of range");
    }
    return tables[tableIndex].table;
}

} // namespace

std::vector<smg::BcsvValue> captureRowValues(const smg::StageArchive& stage, std::size_t tableIndex,
                                             std::size_t rowIndex) {
    if (tableIndex >= stage.tables().size()) {
        throw std::out_of_range("Stage table index is out of range");
    }
    const auto& table = stage.tables()[tableIndex].table;
    if (rowIndex >= table.rows().size()) {
        throw std::out_of_range("Stage table row index is out of range");
    }
    return table.rows()[rowIndex].values;
}

RowEditCommand::RowEditCommand(smg::StageArchive& stage, std::size_t tableIndex, std::size_t rowIndex,
                               std::vector<smg::BcsvValue> before, std::vector<smg::BcsvValue> after,
                               std::string label)
    : stage_(&stage), tableIndex_(tableIndex), rowIndex_(rowIndex), before_(std::move(before)),
      after_(std::move(after)), label_(std::move(label)) {}

void RowEditCommand::restore(const std::vector<smg::BcsvValue>& values) {
    auto& table = tableAt(*stage_, tableIndex_);
    table.setRow(rowIndex_, values);
    stage_->rebuildObjects();
}

TransformCommand::TransformCommand(smg::StageArchive& stage, std::vector<smg::PlacementObject> before,
                                   std::vector<smg::PlacementObject> after, std::string label)
    : stage_(&stage), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

void TransformCommand::apply(const std::vector<smg::PlacementObject>& objects) {
    for (const auto& object : objects) {
        stage_->writeObject(object);
    }
    stage_->rebuildObjects();
}

AddObjectCommand::AddObjectCommand(smg::StageArchive& stage, std::size_t tableIndex, std::size_t rowIndex,
                                   std::vector<smg::BcsvValue> values, std::string label)
    : stage_(&stage), tableIndex_(tableIndex), rowIndex_(rowIndex), values_(std::move(values)),
      label_(std::move(label)) {}

void AddObjectCommand::undo() {
    tableAt(*stage_, tableIndex_).removeRow(rowIndex_);
    stage_->rebuildObjects();
}

void AddObjectCommand::redo() {
    // insertRow returns the index it used; the command already knows it.
    (void)tableAt(*stage_, tableIndex_).insertRow(rowIndex_, values_);
    stage_->rebuildObjects();
}

RemoveObjectCommand::RemoveObjectCommand(smg::StageArchive& stage, std::size_t tableIndex,
                                         std::size_t rowIndex, std::vector<smg::BcsvValue> values,
                                         std::string label)
    : stage_(&stage), tableIndex_(tableIndex), rowIndex_(rowIndex), values_(std::move(values)),
      label_(std::move(label)) {}

void RemoveObjectCommand::undo() {
    // insertRow returns the index it used; the command already knows it.
    (void)tableAt(*stage_, tableIndex_).insertRow(rowIndex_, values_);
    stage_->rebuildObjects();
}

void RemoveObjectCommand::redo() {
    tableAt(*stage_, tableIndex_).removeRow(rowIndex_);
    stage_->rebuildObjects();
}
bool applyRowEdit(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
                  std::size_t rowIndex, std::vector<smg::BcsvValue> after, std::string label) {
    if (tableIndex >= stage.tables().size()) {
        return false;
    }
    if (rowIndex >= stage.tables()[tableIndex].table.rows().size()) {
        return false;
    }
    auto before = captureRowValues(stage, tableIndex, rowIndex);
    stage.tables()[tableIndex].table.setRow(rowIndex, after);
    stage.rebuildObjects();
    stack.push(std::make_unique<RowEditCommand>(stage, tableIndex, rowIndex, std::move(before),
                                                std::move(after), std::move(label)));
    return true;
}

bool applyTransform(smg::StageArchive& stage, UndoStack& stack,
                    std::vector<smg::PlacementObject> before,
                    std::vector<smg::PlacementObject> after, std::string label) {
    if (before.empty() || before.size() != after.size()) {
        return false;
    }
    for (const auto& object : after) {
        stage.writeObject(object);
    }
    stage.rebuildObjects();
    stack.push(std::make_unique<TransformCommand>(stage, std::move(before), std::move(after),
                                                  std::move(label)));
    return true;
}

std::size_t addObject(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
                      std::vector<smg::BcsvValue> values, std::string label,
                      std::optional<std::size_t> insertAt) {
    if (tableIndex >= stage.tables().size()) {
        throw std::out_of_range("Stage table index is out of range");
    }
    auto& table = stage.tables()[tableIndex].table;
    const auto target = insertAt.value_or(table.rows().size());
    const auto rowIndex = table.insertRow(target, values);
    // Snapshot the materialised row so redo reproduces it exactly.
    auto stored = captureRowValues(stage, tableIndex, rowIndex);
    stage.rebuildObjects();
    stack.push(std::make_unique<AddObjectCommand>(stage, tableIndex, rowIndex, std::move(stored),
                                                  std::move(label)));
    return rowIndex;
}

bool removeObject(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
                  std::size_t rowIndex, std::string label) {
    if (tableIndex >= stage.tables().size()) {
        return false;
    }
    auto& table = stage.tables()[tableIndex].table;
    if (rowIndex >= table.rows().size()) {
        return false;
    }
    auto values = captureRowValues(stage, tableIndex, rowIndex);
    table.removeRow(rowIndex);
    stage.rebuildObjects();
    stack.push(std::make_unique<RemoveObjectCommand>(stage, tableIndex, rowIndex, std::move(values),
                                                     std::move(label)));
    return true;
}

bool mutateRow(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
               std::size_t rowIndex,
               const std::function<void(smg::BcsvTable&, smg::BcsvRow&)>& mutate, std::string label) {
    if (tableIndex >= stage.tables().size()) {
        return false;
    }
    auto& table = stage.tables()[tableIndex].table;
    if (rowIndex >= table.rows().size()) {
        return false;
    }
    auto before = captureRowValues(stage, tableIndex, rowIndex);
    mutate(table, table.rows()[rowIndex]);
    auto after = captureRowValues(stage, tableIndex, rowIndex);
    if (before == after) {
        return false;
    }
    stage.rebuildObjects();
    stack.push(std::make_unique<RowEditCommand>(stage, tableIndex, rowIndex, std::move(before),
                                                std::move(after), std::move(label)));
    return true;
}

CameraTableCommand::CameraTableCommand(smg::StageArchive& stage, std::vector<std::uint8_t> before,
                                       std::vector<std::uint8_t> after, std::string label)
    : stage_(&stage), before_(std::move(before)), after_(std::move(after)),
      label_(std::move(label)) {}

void CameraTableCommand::restore(const std::vector<std::uint8_t>& bytes) {
    // Re-parse in the archive's own endianness so an SMG1 table restored from
    // an undo snapshot is byte-identical to the file it came from.
    stage_->setCameraParams(smg::CameraParamTable(bytes, stage_->endian(),
                                                  stage_->cameraDefaultVersion()));
}

bool mutateCameras(smg::StageArchive& stage, UndoStack& stack,
                   const std::function<void(smg::CameraParamTable&)>& mutate, std::string label) {
    auto before = stage.cameraParams().serialize();
    mutate(stage.cameraParams());
    auto after = stage.cameraParams().serialize();
    if (before == after) {
        return false;
    }
    stack.push(std::make_unique<CameraTableCommand>(stage, std::move(before), std::move(after),
                                                     std::move(label)));
    return true;
}

GalaxyTableCommand::GalaxyTableCommand(smg::GalaxyArchive& galaxy,
                                       std::vector<std::uint8_t> scenarioBefore,
                                       std::vector<std::uint8_t> zoneBefore,
                                       std::vector<std::uint8_t> scenarioAfter,
                                       std::vector<std::uint8_t> zoneAfter, std::string label)
    : galaxy_(&galaxy), scenarioBefore_(std::move(scenarioBefore)),
      zoneBefore_(std::move(zoneBefore)), scenarioAfter_(std::move(scenarioAfter)),
      zoneAfter_(std::move(zoneAfter)), label_(std::move(label)) {}

void GalaxyTableCommand::restore(const std::vector<std::uint8_t>& scenarioBytes,
                                 const std::vector<std::uint8_t>& zoneBytes) {
    // Re-parsed in the archive's own endianness so a restored table is byte-
    // identical to the file it came from, exactly as CameraTableCommand does for
    // the camera table. setZoneList also rebuilds the cached zone-name list, so
    // the Project panel follows an undo that added or removed a zone.
    galaxy_->setScenarioData(smg::BcsvTable(scenarioBytes, galaxy_->endian()));
    galaxy_->setZoneList(smg::BcsvTable(zoneBytes, galaxy_->endian()));
}

void GalaxyTableCommand::undo() { restore(scenarioBefore_, zoneBefore_); }
void GalaxyTableCommand::redo() { restore(scenarioAfter_, zoneAfter_); }

bool mutateScenarios(smg::GalaxyArchive& galaxy, UndoStack& stack,
                     const std::function<void(smg::BcsvTable&, smg::BcsvTable&)>& mutate,
                     std::string label) {
    const auto scenarioBefore = galaxy.scenarioData().serialize();
    const auto zoneBefore = galaxy.zoneList().serialize();
    mutate(galaxy.scenarioData(), galaxy.zoneList());
    const auto scenarioAfter = galaxy.scenarioData().serialize();
    const auto zoneAfter = galaxy.zoneList().serialize();
    if (scenarioBefore == scenarioAfter && zoneBefore == zoneAfter) {
        return false;
    }
    // The lambda got raw tables, so it could have added or removed a zone row
    // without going through GalaxyArchive. Re-installing the changed tables is
    // what rebuilds the cached zone-name list; without this the Project panel
    // would keep listing zones the table no longer has. Cheap: the tables are a
    // few hundred bytes and the cache is a handful of strings.
    galaxy.setScenarioData(smg::BcsvTable(scenarioAfter, galaxy.endian()));
    galaxy.setZoneList(smg::BcsvTable(zoneAfter, galaxy.endian()));
    stack.push(std::make_unique<GalaxyTableCommand>(
        galaxy, scenarioBefore, zoneBefore, scenarioAfter, zoneAfter, std::move(label)));
    return true;
}

} // namespace whitehole::edit
