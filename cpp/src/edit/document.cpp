#include "whitehole/edit/document.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace whitehole::edit {

Document::Document() = default;
Document::~Document() = default;

void Document::reset() {
    game_.reset();
    galaxy_.reset();
    stage_.reset();
    galaxyName_.clear();
    zoneName_.clear();
    selection_.clear();
    undo_.clear();
    savedUndoCursor_ = 0;
    dirty_ = false;
    fromGameDirectory_ = false;
}

void Document::close() {
    reset();
    notifyObservers();
}

void Document::openMapFile(const std::filesystem::path& archive, int gameType) {
    auto opened = std::make_unique<smg::StageArchive>(smg::StageArchive::openMapFile(archive, gameType));
    reset();
    stage_ = std::move(opened);
    gameType_ = gameType;
    zoneName_ = stage_->stageName();
    markSaved();
    notifyObservers();
}

void Document::openGameDirectory(const std::filesystem::path& root) {
    auto game = std::make_unique<smg::GameArchive>(root);
    reset();
    game_ = std::move(game);
    gameType_ = game_->gameType();
    fromGameDirectory_ = true;
    markSaved();
    notifyObservers();
}

bool Document::openGalaxy(std::string_view galaxy) {
    if (game_ == nullptr) {
        return false;
    }
    auto opened = std::make_unique<smg::GalaxyArchive>(game_->openGalaxy(galaxy));
    galaxy_ = std::move(opened);
    galaxyName_ = galaxy_->name();
    // The previous zone belonged to the previous galaxy.
    stage_.reset();
    zoneName_.clear();
    selection_.clear();
    undo_.clear();
    markSaved();
    notifyObservers();
    return true;
}

bool Document::openZone(std::string_view zone) {
    if (galaxy_ == nullptr) {
        return false;
    }
    auto opened = std::make_unique<smg::StageArchive>(galaxy_->openZone(zone));
    stage_ = std::move(opened);
    zoneName_ = stage_->stageName();
    selection_.clear();
    undo_.clear();
    markSaved();
    notifyObservers();
    return true;
}

smg::ObjectModel Document::objectModel() {
    if (stage_ == nullptr || database_ == nullptr) {
        // A model over an empty scratch stage keeps callers from branching.
        return smg::ObjectModel(scratchStage_, scratchDatabase_, gameType_);
    }
    return smg::ObjectModel(*stage_, *database_, gameType_);
}

void Document::select(std::size_t objectIndex, bool additive) {
    if (!additive) {
        selection_.clear();
        selection_.push_back(objectIndex);
    } else if (!isSelected(objectIndex)) {
        selection_.push_back(objectIndex);
    }
    notifyObservers();
}

void Document::setSelection(std::vector<std::size_t> objects) {
    selection_ = std::move(objects);
    notifyObservers();
}

void Document::clearSelection() noexcept {
    selection_.clear();
}

bool Document::isSelected(std::size_t objectIndex) const {
    return std::find(selection_.begin(), selection_.end(), objectIndex) != selection_.end();
}

bool Document::undo() {
    if (!undo_.undo()) {
        return false;
    }
    // The object count can change under an add/delete, so drop stale indices.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [this](std::size_t index) {
        return stage_ == nullptr || index >= stage_->objects().size();
    }), selection_.end());
    notifyObservers();
    return true;
}

bool Document::redo() {
    if (!undo_.redo()) {
        return false;
    }
    notifyObservers();
    return true;
}

void Document::notifyChanged() {
    // A direct mutation means something changed and we cannot know whether it
    // went through the undo commands, so mark dirty as well as notifying.
    dirty_ = true;
    notifyObservers();
}

void Document::notifyObservers() {
    if (onChange_) {
        onChange_();
    }
}

void Document::markSaved() noexcept {
    savedUndoCursor_ = undo_.cursor();
    dirty_ = false;
}

std::filesystem::path Document::sourcePath() const {
    if (stage_ == nullptr) {
        return {};
    }
    return stage_->sourcePath();
}

void Document::save() {
    if (stage_ == nullptr) {
        throw std::runtime_error("Nothing is open to save");
    }
    stage_->save();
    markSaved();
    notifyObservers();
}

void Document::saveAs(const std::filesystem::path& path) {
    if (stage_ == nullptr) {
        throw std::runtime_error("Nothing is open to save");
    }
    stage_->saveTo(path);
    markSaved();
    notifyObservers();
}

ValidationReport Document::validate() const {
    if (stage_ == nullptr || database_ == nullptr) {
        return {};
    }
    return validateStage(*stage_, *database_, gameType_, customObjects_);
}

} // namespace whitehole::edit