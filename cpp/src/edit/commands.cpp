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

} // namespace whitehole::edit
