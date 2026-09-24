#include "whitehole/db/custom_obj_db.hpp"

#include "whitehole/db/object_db.hpp"
#include "whitehole/util/json.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <time.h>

namespace whitehole::db {
namespace {

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(time(nullptr));
}

} // namespace

void CustomObjDatabase::load(const std::filesystem::path& path) {
    clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return; // no file yet: an empty database
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open custom object database: " + path.string());
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    loadFromJson(text);
    // Remember the on-disk form so an unchanged save() is a no-op.
    lastSavedJson_ = text;
    loaded_ = true;
}

void CustomObjDatabase::loadFromJson(std::string_view text) {
    clear();
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return; // empty document: an empty database
    }
    const util::JsonValue root = util::parseJson(text);
    for (const auto& item : root.at("Objects").asArray()) {
        CustomObjectEntry entry;
        entry.name = item.at("Name").asString();
        if (entry.name.empty()) {
            continue; // a nameless entry cannot be looked up or previewed
        }
        entry.displayName = item.at("DisplayName").asString();
        entry.modelPath = item.at("Model").asString();
        entry.notes = item.at("Notes").asString();
        entry.source = item.at("Source").asString();
        if (const auto added = item.at("Added"); added.isNumber()) {
            entry.addedAt = static_cast<std::int64_t>(added.asNumber());
        }
        // Duplicate names: last one wins, matching the lookup map below.
        if (const auto existing = lookup_.find(entry.name); existing != lookup_.end()) {
            entries_[existing->second] = std::move(entry);
            continue;
        }
        lookup_.emplace(entry.name, entries_.size());
        entries_.push_back(std::move(entry));
    }
}

std::string CustomObjDatabase::toJson() const {
    util::JsonArray objects;
    objects.reserve(entries_.size());
    for (const auto& entry : entries_) {
        util::JsonObject item;
        item["Name"] = util::JsonValue(entry.name);
        item["DisplayName"] = util::JsonValue(entry.displayName);
        item["Model"] = util::JsonValue(entry.modelPath);
        item["Notes"] = util::JsonValue(entry.notes);
        item["Source"] = util::JsonValue(entry.source);
        item["Added"] = util::JsonValue(static_cast<double>(entry.addedAt));
        objects.emplace_back(util::JsonValue(std::move(item)));
    }
    util::JsonObject root;
    root["Objects"] = util::JsonValue(std::move(objects));
    return util::serializeJson(util::JsonValue(std::move(root)));
}

void CustomObjDatabase::save(const std::filesystem::path& path) {
    const std::string text = toJson();
    if (loaded_ && text == lastSavedJson_) {
        return; // nothing changed since the last load/save
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Cannot write custom object database: " + path.string());
    }
    stream << text;
    if (!stream) {
        throw std::runtime_error("Failed while writing custom object database: " + path.string());
    }
    lastSavedJson_ = text;
    loaded_ = true;
}

void CustomObjDatabase::clear() noexcept {
    entries_.clear();
    lookup_.clear();
    lastSavedJson_.clear();
    loaded_ = false;
}

bool CustomObjDatabase::contains(std::string_view name) const {
    return lookup_.find(std::string(name)) != lookup_.end();
}

const CustomObjectEntry* CustomObjDatabase::find(std::string_view name) const {
    const auto found = lookup_.find(std::string(name));
    if (found == lookup_.end()) {
        return nullptr;
    }
    return &entries_[found->second];
}


bool CustomObjDatabase::add(CustomObjectEntry entry) {
    if (entry.name.empty()) {
        return false;
    }
    if (entry.addedAt == 0) {
        entry.addedAt = nowSeconds();
    }
    const auto existing = lookup_.find(entry.name);
    if (existing != lookup_.end()) {
        CustomObjectEntry& current = entries_[existing->second];
        // Keep the original registration time and source; refresh the rest.
        entry.addedAt = current.addedAt;
        if (entry.source.empty()) {
            entry.source = current.source;
        }
        const bool same = current.displayName == entry.displayName &&
                          current.modelPath == entry.modelPath &&
                          current.notes == entry.notes && current.source == entry.source;
        if (same) {
            return false; // identical: no change
        }
        current = std::move(entry);
        return true;
    }
    lookup_.emplace(entry.name, entries_.size());
    entries_.push_back(std::move(entry));
    return true;
}

bool CustomObjDatabase::remove(std::string_view name) {
    const auto found = lookup_.find(std::string(name));
    if (found == lookup_.end()) {
        return false;
    }
    const std::size_t index = found->second;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    lookup_.erase(found);
    reindex();
    return true;
}

bool CustomObjDatabase::setModelPath(std::string_view name, std::string modelPath) {
    if (find(name) == nullptr) {
        CustomObjectEntry created;
        created.name = std::string(name);
        created.modelPath = std::move(modelPath);
        return add(std::move(created));
    }
    CustomObjectEntry updated = *find(name);
    if (updated.modelPath == modelPath) {
        return false;
    }
    updated.modelPath = std::move(modelPath);
    return add(std::move(updated));
}


CustomObjSyncOutcome CustomObjDatabase::syncFromObjectSection(
    std::string_view source, const std::vector<std::string>& namesPresent,
    const ObjectDatabase* official) {
    CustomObjSyncOutcome outcome;
    const std::string sourceId(source);

    // Adds: only names the vanilla database does not know qualify as custom,
    // and only when the vanilla database is actually installed. Without it
    // there is no way to tell a custom object from a shipped one, and guessing
    // would flood this file with thousands of vanilla names.
    const bool officialReady = official != nullptr && !official->empty();
    if (officialReady) {
        for (const auto& name : namesPresent) {
            if (name.empty() || official->contains(name)) {
                continue;
            }
            if (contains(name)) {
                // Already known. An entry created by hand ("assign a model"
                // before the first sync) has no source yet, so this table
                // adopts it -- otherwise it would never be cleaned up when the
                // name is later deleted from here.
                if (const auto* existing = find(name); existing != nullptr && existing->source.empty()) {
                    CustomObjectEntry adopted = *existing;
                    adopted.source = sourceId;
                    if (add(std::move(adopted))) {
                        outcome.adopted.push_back(name);
                    }
                }
                continue;
            }
            CustomObjectEntry entry;
            entry.name = name;
            entry.source = sourceId;
            if (add(std::move(entry))) {
                outcome.added.push_back(name);
            }
        }
    }

    // Removals: drop entries THIS source registered whose name vanished from
    // it. Entries registered by another table are untouched by definition.
    std::vector<std::string> doomed;
    for (const auto& entry : entries_) {
        if (entry.source != sourceId) {
            continue;
        }
        if (std::find(namesPresent.begin(), namesPresent.end(), entry.name) == namesPresent.end()) {
            doomed.push_back(entry.name);
        }
    }
    for (const auto& name : doomed) {
        if (remove(name)) {
            outcome.removed.push_back(name);
        }
    }
    return outcome;
}

void CustomObjDatabase::reindex() noexcept {
    lookup_.clear();
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        lookup_[entries_[index].name] = index;
    }
}

} // namespace whitehole::db

