#pragma once

// Concrete undo commands for stage editing. Each command snapshots exactly what
// it needs to restore, and every change re-derives the placement list from the
// tables, so undo can never leave objects() out of step with the BCSV data.
//
// The free helper functions at the bottom are the intended entry points: they
// capture the "before" state, apply the change, and record it on the stack in
// one call, which is what the GUI, the CLI and the tests all need.

#include "whitehole/edit/undo.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/placement.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace whitehole::edit {

// Full serialized row. Throws std::out_of_range when the table/row is missing.
[[nodiscard]] std::vector<smg::BcsvValue> captureRowValues(const smg::StageArchive& stage,
                                                          std::size_t tableIndex, std::size_t rowIndex);

// Replaces one table row wholesale: args, switches, name, or any BCSV field.
class RowEditCommand final : public IUndo {
public:
    RowEditCommand(smg::StageArchive& stage, std::size_t tableIndex, std::size_t rowIndex,
                   std::vector<smg::BcsvValue> before, std::vector<smg::BcsvValue> after,
                   std::string label);

    void undo() override { restore(before_); }
    void redo() override { restore(after_); }
    [[nodiscard]] std::string label() const override { return label_; }

private:
    void restore(const std::vector<smg::BcsvValue>& values);

    smg::StageArchive* stage_;
    std::size_t tableIndex_;
    std::size_t rowIndex_;
    std::vector<smg::BcsvValue> before_;
    std::vector<smg::BcsvValue> after_;
    std::string label_;
};

// Transform-only edit over one or more objects (the drag/rotate/scale case).
// Lighter than a row snapshot because it only touches name + pos/rot/scale.
class TransformCommand final : public IUndo {
public:
    TransformCommand(smg::StageArchive& stage, std::vector<smg::PlacementObject> before,
                     std::vector<smg::PlacementObject> after, std::string label);

    void undo() override { apply(before_); }
    void redo() override { apply(after_); }
    [[nodiscard]] std::string label() const override { return label_; }

private:
    void apply(const std::vector<smg::PlacementObject>& objects);

    smg::StageArchive* stage_;
    std::vector<smg::PlacementObject> before_;
    std::vector<smg::PlacementObject> after_;
    std::string label_;
};

// Inserts an object row. undo() removes it again.
class AddObjectCommand final : public IUndo {
public:
    AddObjectCommand(smg::StageArchive& stage, std::size_t tableIndex, std::size_t rowIndex,
                     std::vector<smg::BcsvValue> values, std::string label);

    void undo() override;
    void redo() override;
    [[nodiscard]] std::string label() const override { return label_; }
    [[nodiscard]] std::size_t rowIndex() const noexcept { return rowIndex_; }

private:
    smg::StageArchive* stage_;
    std::size_t tableIndex_;
    std::size_t rowIndex_;
    std::vector<smg::BcsvValue> values_;
    std::string label_;
};

// Deletes an object row. undo() re-inserts it at the same index.
class RemoveObjectCommand final : public IUndo {
public:
    RemoveObjectCommand(smg::StageArchive& stage, std::size_t tableIndex, std::size_t rowIndex,
                        std::vector<smg::BcsvValue> values, std::string label);

    void undo() override;
    void redo() override;
    [[nodiscard]] std::string label() const override { return label_; }

private:
    smg::StageArchive* stage_;
    std::size_t tableIndex_;
    std::size_t rowIndex_;
    std::vector<smg::BcsvValue> values_;
    std::string label_;
};

// ---- helpers: apply + record -------------------------------------------
// Each returns false (and records nothing) when the target indices are stale.
// The caller is responsible for setting `after` on the objects first when using
// applyTransform, exactly like a drag in the viewport.

[[nodiscard]] bool applyRowEdit(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
                                std::size_t rowIndex, std::vector<smg::BcsvValue> after,
                                std::string label);

[[nodiscard]] bool applyTransform(smg::StageArchive& stage, UndoStack& stack,
                                 std::vector<smg::PlacementObject> before,
                                 std::vector<smg::PlacementObject> after, std::string label);

// Appends an object row and returns its new row index, or nullopt on failure.
// `insertAt` places the row before that index instead of at the end (used by
// duplicate, which keeps the copy next to its original); out-of-range values are
// clamped to the end by BcsvTable::insertRow.
[[nodiscard]] std::size_t addObject(smg::StageArchive& stage, UndoStack& stack,
                                   std::size_t tableIndex, std::vector<smg::BcsvValue> values,
                                   std::string label,
                                   std::optional<std::size_t> insertAt = std::nullopt);

[[nodiscard]] bool removeObject(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
                               std::size_t rowIndex, std::string label);


// Runs `mutate` against one row and records the before/after pair as a single
// undo step. Records nothing (and returns false) when the row did not change,
// so a drag gesture that ends where it started leaves the stack clean. The
// rail editors use this to edit arbitrary BCSV fields without one command
// class per field.
[[nodiscard]] bool mutateRow(smg::StageArchive& stage, UndoStack& stack, std::size_t tableIndex,
                             std::size_t rowIndex,
                             const std::function<void(smg::BcsvTable&, smg::BcsvRow&)>& mutate,
                             std::string label);

} // namespace whitehole::edit