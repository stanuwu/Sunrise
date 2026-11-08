#include "mod_problem.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <span>

namespace sunrise::state::mods::problem {
namespace {

/**
 * Retains the first refusal even when warnings have filled the problem list.
 * @param record Mod the problem is about.
 * @param reason Machine key for the log.
 * @param severity Refusal or warning.
 * @param format printf-style message.
 * @param arguments Message arguments.
 */
void add(Record& record,
         Reason reason,
         Severity severity,
         const char* format,
         std::va_list arguments) noexcept {
    if (severity == Severity::refusal) {
        record.status = Status::refused;
    }
    std::size_t index = record.problemCount;
    if (index == record.problems.size()) {
        ++record.droppedProblems;
        if (severity != Severity::refusal || first_refusal(record) != nullptr) {
            return;
        }
        --index;
    } else {
        ++record.problemCount;
    }
    Problem& problem = record.problems[index];
    problem = {};
    problem.reason = reason;
    problem.severity = severity;
    const int written = std::vsnprintf(
        problem.message.bytes.data(), problem.message.bytes.size(), format, arguments);
    problem.message.length = written > 0 ? (std::min)(static_cast<std::size_t>(written),
                                                      problem.message.bytes.size() - 1)
                                         : 0;
}

} // namespace

/** Records one refusal and keeps the mod out. */
void refuse(Record& record, Reason reason, const char* format, ...) noexcept {
    std::va_list arguments;
    va_start(arguments, format);
    add(record, reason, Severity::refusal, format, arguments);
    va_end(arguments);
}

/** Records one warning. The mod still loads. */
void warn(Record& record, Reason reason, const char* format, ...) noexcept {
    std::va_list arguments;
    va_start(arguments, format);
    add(record, reason, Severity::warning, format, arguments);
    va_end(arguments);
}

/** @return The stable lowercase log key for one reason. */
const char* reason_name(Reason reason) noexcept {
    switch (reason) {
    case Reason::manifestMissing:
        return "manifest_missing";
    case Reason::manifestUnreadable:
        return "manifest_unreadable";
    case Reason::manifestInvalid:
        return "manifest_invalid";
    case Reason::manifestVersion:
        return "manifest_version";
    case Reason::duplicateId:
        return "duplicate_id";
    case Reason::capacity:
        return "capacity";
    case Reason::folderPath:
        return "folder_path";
    case Reason::missingDependency:
        return "missing_dependency";
    case Reason::dependencyVersion:
        return "dependency_version";
    case Reason::dependencyNotLoaded:
        return "dependency_not_loaded";
    case Reason::dependencyCycle:
        return "dependency_cycle";
    case Reason::incompatible:
        return "incompatible";
    case Reason::packageInvalid:
        return "package_invalid";
    case Reason::packageConflict:
        return "package_conflict";
    case Reason::patchTarget:
        return "patch_target";
    case Reason::patchInvalid:
        return "patch_invalid";
    case Reason::mergeFailed:
        return "merge_failed";
    case Reason::assetOverride:
        return "asset_override";
    case Reason::packageRemapped:
        return "package_remapped";
    }
    return "unknown";
}

/** @return The first refusal recorded, or null when the mod loads. */
const Problem* first_refusal(const Record& record) noexcept {
    const auto problems = std::span(record.problems).first(record.problemCount);
    const auto found = std::find_if(problems.begin(), problems.end(), [](const Problem& entry) {
        return entry.severity == Severity::refusal;
    });
    return found == problems.end() ? nullptr : &*found;
}

} // namespace sunrise::state::mods::problem
