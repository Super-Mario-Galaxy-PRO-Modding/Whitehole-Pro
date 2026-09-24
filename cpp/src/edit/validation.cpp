#include "whitehole/edit/validation.hpp"

#include "whitehole/db/custom_obj_db.hpp"

#include <unordered_set>
#include <utility>

namespace whitehole::edit {
namespace {

// Local integer extraction so validation never depends on BCSV internals.
bool asInteger(const smg::BcsvValue& value, std::int32_t& out) {
    if (const auto* item = std::get_if<std::int32_t>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<std::int16_t>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<std::int8_t>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<float>(&value)) { out = static_cast<std::int32_t>(*item); return true; }
    return false;
}

bool isUnset(const smg::BcsvValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return text->empty();
    }
    std::int32_t integer = 0;
    return asInteger(value, integer) && integer == 0;
}

void add(ValidationReport& report, Severity severity, std::string code, std::string message,
         std::string hint, std::optional<std::size_t> objectIndex, std::string field) {
    report.findings.push_back(Finding{severity, std::move(code), std::move(message), std::move(hint),
                                      objectIndex, std::move(field)});
}

} // namespace

std::string_view toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return "info";
}

std::size_t ValidationReport::count(Severity severity) const noexcept {
    std::size_t total = 0;
    for (const auto& finding : findings) {
        if (finding.severity == severity) {
            ++total;
        }
    }
    return total;
}

bool ValidationReport::clean() const noexcept {
    for (const auto& finding : findings) {
        if (finding.severity == Severity::Warning || finding.severity == Severity::Error) {
            return false;
        }
    }
    return true;
}

ValidationReport validateStage(const smg::StageArchive& stage, const db::ObjectDatabase& database,
                               int gameType, const db::CustomObjDatabase* customObjects) {
    ValidationReport report;
    if (database.empty()) {
        return report; // Nothing to validate against; stay quiet rather than guess.
    }

    // ObjectModel wants a mutable stage, but every rule here is read-only.
    auto& mutableStage = const_cast<smg::StageArchive&>(stage);
    smg::ObjectModel model(mutableStage, database, gameType);

    for (std::size_t index = 0; index < stage.objects().size(); ++index) {
        const auto& object = stage.objects()[index];
        const auto* info = database.find(object.name);
        if (info == nullptr) {
            // A name the modder registered in the CustomObjDatabase is their own
            // object, not a typo -- the BCSV editor put it there on purpose.
            const bool isCustom = customObjects != nullptr && customObjects->contains(object.name);
            if (!isCustom) {
                add(report, Severity::Warning, "unknown-object",
                    "Object \"" + object.name + "\" is not in the object database.",
                    "Replace it with a known object, or update data/objectdb.json.", index, {});
            }
        } else if (!database.objectAvailable(object.name, gameType)) {
            add(report, Severity::Warning, "game-mismatch",
                "Object \"" + object.name + "\" does not exist in this game.",
                gameType == 1 ? "Use an SMG1 object or switch the project to SMG2."
                              : "Use an SMG2 object or switch the project to SMG1.",
                index, {});
        }

        if (object.scale.x == 0.0F && object.scale.y == 0.0F && object.scale.z == 0.0F) {
            add(report, Severity::Info, "zero-scale",
                "Object \"" + object.name + "\" has a zero scale, so it will not be visible.",
                "Set a non-zero scale, or 1,1,1 for the default size.", index, "scale_x");
        }

        const auto fields = model.fields(index);
        std::unordered_set<std::string> present;
        present.reserve(fields.size());
        for (const auto& field : fields) {
            if (field.present) {
                present.insert(field.identifier);
            }
            if (!field.present) {
                continue;
            }
            if (field.needed && isUnset(field.value)) {
                add(report, Severity::Info, "unset-required",
                    "Required parameter \"" + field.label + "\" is not set.",
                    "Open the property panel and give it a value.", index, field.identifier);
            }
            if (field.kind == db::PropertyKind::SwitchId) {
                std::int32_t switchId = 0;
                if (asInteger(field.value, switchId) && switchId == 0) {
                    add(report, Severity::Warning, "dangling-switch",
                        "\"" + field.label + "\" references switch 0, which does not exist.",
                        "Point it at a real switch, or leave the object out of the scene.",
                        index, field.identifier);
                }
            }
        }

        // A class parameter the database marks as required but the row never
        // stores means the object cannot be configured at all.
        if (const auto* objectClass = model.objectClass(index)) {
            for (const auto& entry : objectClass->properties) {
                const auto& property = entry.second;
                if (!property.needed || present.count(entry.first) != 0) {
                    continue;
                }
                add(report, Severity::Warning, "missing-required",
                    "Required parameter \"" +
                        (property.simpleName.empty() ? entry.first : property.simpleName) +
                        "\" is missing from this object's data.",
                    "Recreate the object from the Add Object panel so the field exists.",
                    index, entry.first);
            }
        }
    }
    return report;
}

} // namespace whitehole::edit