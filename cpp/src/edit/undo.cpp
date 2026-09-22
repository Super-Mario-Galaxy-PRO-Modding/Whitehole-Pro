#include "whitehole/edit/undo.hpp"

#include <utility>

namespace whitehole::edit {

void UndoStack::push(UndoEntry entry) {
    if (entry == nullptr) {
        return;
    }
    if (group_ != nullptr) {
        // Inside a group capture the stack is untouched until endGroup(); the
        // redo branch is discarded there, exactly once.
        group_->add(std::move(entry));
        return;
    }
    // Drop the redo branch: a new action replaces any undone future.
    entries_.resize(cursor_);
    entries_.push_back(std::move(entry));
    cursor_ = entries_.size();
}

void UndoStack::beginGroup(std::string label) {
    if (group_ != nullptr) {
        return; // one level only: swallowing the outer capture would be worse
    }
    group_ = std::make_unique<UndoMultiEntry>(std::move(label));
}

void UndoStack::endGroup() {
    if (group_ == nullptr) {
        return;
    }
    auto group = std::move(group_);
    if (group->empty()) {
        return; // nothing was ever recorded: the stack stays untouched
    }
    entries_.resize(cursor_);
    entries_.push_back(std::move(group));
    cursor_ = entries_.size();
}

bool UndoStack::undo() {
    if (!canUndo()) {
        return false;
    }
    --cursor_;
    entries_[cursor_]->undo();
    return true;
}

bool UndoStack::redo() {
    if (!canRedo()) {
        return false;
    }
    entries_[cursor_]->redo();
    ++cursor_;
    return true;
}

std::string UndoStack::undoLabel() const {
    if (!canUndo()) {
        return {};
    }
    return entries_[cursor_ - 1]->label();
}

std::string UndoStack::redoLabel() const {
    if (!canRedo()) {
        return {};
    }
    return entries_[cursor_]->label();
}

void UndoStack::clear() noexcept {
    entries_.clear();
    cursor_ = 0;
    group_.reset(); // a capture cannot outlive the history it belongs to
}

UndoMultiEntry::UndoMultiEntry(std::string label) : label_(std::move(label)) {}

void UndoMultiEntry::add(UndoEntry entry) {
    if (entry == nullptr) {
        return;
    }
    entries_.push_back(std::move(entry));
}

void UndoMultiEntry::undo() {
    // Reverse order so a later child that depended on an earlier one unwinds
    // first.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        (*it)->undo();
    }
}

void UndoMultiEntry::redo() {
    for (auto& entry : entries_) {
        entry->redo();
    }
}

std::string UndoMultiEntry::label() const {
    return label_;
}

} // namespace whitehole::edit