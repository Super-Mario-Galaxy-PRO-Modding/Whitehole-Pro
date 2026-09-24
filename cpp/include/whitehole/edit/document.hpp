#pragma once

// The document model: the single owner of "what is open right now" -- the game
// workspace, the galaxy, the zone, the selection, the undo history, the dirty
// flag and validation. The desktop editor renders it, the CLI drives it, and
// the tests exercise it, all without any Windows or OpenGL code.
//
// Before this existed the same state lived inside the 900-line Win32 editor
// state, which is why undo, the close-confirmation guard and validation could
// not be added. Everything now routes through one object.
//
// Contract for edits: mutate through stage() (directly or via objectModel()),
// then call notifyChanged(). notifyChanged() sets the dirty flag and fires the
// change callback so panels refresh. undo()/redo() call it for you.

#include "whitehole/db/object_db.hpp"
#include "whitehole/edit/commands.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/edit/validation.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/galaxy_archive.hpp"
#include "whitehole/smg/object_model.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::edit {

class Document {
public:
    Document();
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) = delete;
    Document& operator=(Document&&) = delete;

    // ---- opening ---------------------------------------------------------
    // Opens a standalone map archive (.arc/.szs). Works without a game dump.
    void openMapFile(const std::filesystem::path& archive, int gameType = 2);
    // Opens an extracted game workspace (the folder containing StageData).
    void openGameDirectory(const std::filesystem::path& root);
    // Opens a galaxy inside the current workspace. False when it does not exist.
    bool openGalaxy(std::string_view galaxy);
    // Opens a zone inside the current galaxy. False when it does not exist.
    bool openZone(std::string_view zone);
    // Drops everything and returns to the empty-editor state.
    void close();

    [[nodiscard]] bool hasStage() const noexcept { return stage_ != nullptr; }
    [[nodiscard]] bool hasGalaxy() const noexcept { return galaxy_ != nullptr; }
    [[nodiscard]] smg::StageArchive* stage() noexcept { return stage_.get(); }
    [[nodiscard]] const smg::StageArchive* stage() const noexcept { return stage_.get(); }
    [[nodiscard]] smg::GameArchive* game() noexcept { return game_.get(); }
    [[nodiscard]] const smg::GalaxyArchive* galaxy() const noexcept { return galaxy_.get(); }
    [[nodiscard]] const std::string& galaxyName() const noexcept { return galaxyName_; }
    [[nodiscard]] const std::string& zoneName() const noexcept { return zoneName_; }
    [[nodiscard]] bool fromGameDirectory() const noexcept { return fromGameDirectory_; }

    [[nodiscard]] int gameType() const noexcept { return gameType_; }
    void setGameType(int gameType) noexcept { gameType_ = gameType; }

    // ---- object database -------------------------------------------------
    void setDatabase(const db::ObjectDatabase* database) noexcept { database_ = database; }
    [[nodiscard]] const db::ObjectDatabase* database() const noexcept { return database_; }
    // Registry of the modder's own objects (names objectdb.json does not know).
    // The BCSV editor keeps it in step with object sections; validation uses it
    // so a custom object is never reported as an unknown one. The pointer is
    // non-owning: the caller (the app) keeps the database alive.
    void setCustomObjects(const db::CustomObjDatabase* customObjects) noexcept {
        customObjects_ = customObjects;
    }
    [[nodiscard]] const db::CustomObjDatabase* customObjects() const noexcept { return customObjects_; }
    // Object view over the open stage. Empty (0 objects) when nothing is open.
    [[nodiscard]] smg::ObjectModel objectModel();

    // ---- selection -------------------------------------------------------
    [[nodiscard]] const std::vector<std::size_t>& selection() const noexcept { return selection_; }
    void select(std::size_t objectIndex, bool additive = false);
    void setSelection(std::vector<std::size_t> objects);
    void clearSelection() noexcept;
    [[nodiscard]] bool isSelected(std::size_t objectIndex) const;

    // ---- undo / redo -----------------------------------------------------
    [[nodiscard]] UndoStack& undoStack() noexcept { return undo_; }
    [[nodiscard]] bool canUndo() const noexcept { return undo_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return undo_.canRedo(); }
    [[nodiscard]] std::string undoLabel() const { return undo_.undoLabel(); }
    [[nodiscard]] std::string redoLabel() const { return undo_.redoLabel(); }
    bool undo();
    bool redo();

    // ---- dirty state -----------------------------------------------------
    // Dirty when the undo cursor has moved away from the last save point, so
    // undoing back to the saved state reports clean again. markDirty() covers
    // the rare case of a mutation that bypasses the undo commands.
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_ || undo_.cursor() != savedUndoCursor_;
    }
    void markDirty() noexcept { dirty_ = true; }
    // Call after any direct stage mutation. Edits made through the undo
    // commands (or ObjectModel's setters) are detected automatically.
    void notifyChanged();

    // ---- saving ----------------------------------------------------------
    // Saves in place (back to the game workspace or the opened archive).
    void save();
    void saveAs(const std::filesystem::path& path);
    [[nodiscard]] std::filesystem::path sourcePath() const;

    // ---- validation ------------------------------------------------------
    [[nodiscard]] ValidationReport validate() const;

    // ---- change notifications -------------------------------------------
    using ChangeCallback = std::function<void()>;
    void setOnChange(ChangeCallback callback) { onChange_ = std::move(callback); }

private:
    void reset();
    void notifyObservers();
    // Records the current undo position as "saved", clearing the dirty state.
    void markSaved() noexcept;

    std::unique_ptr<smg::GameArchive> game_;
    std::unique_ptr<smg::GalaxyArchive> galaxy_;
    std::unique_ptr<smg::StageArchive> stage_;
    // Scratch objects so objectModel() never has to return a null reference.
    smg::StageArchive scratchStage_;
    db::ObjectDatabase scratchDatabase_;
    const db::ObjectDatabase* database_{nullptr};
    const db::CustomObjDatabase* customObjects_{nullptr};
    std::string galaxyName_;
    std::string zoneName_;
    std::vector<std::size_t> selection_;
    UndoStack undo_;
    std::size_t savedUndoCursor_{0};
    int gameType_{2};
    bool dirty_{false};
    bool fromGameDirectory_{false};
    ChangeCallback onChange_;
};

} // namespace whitehole::edit