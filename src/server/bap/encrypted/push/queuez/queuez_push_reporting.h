#pragma once

#include <cstddef>

namespace sunrise::server::bap::encrypted::push::queuez_report {

/** Passed as the recorded flag when the stage has no record outcome to report. */
inline constexpr int kNoRecordOutcome = -1;

/**
 * Reports one appended push with the fields a reference capture can be compared against.
 * Object count and framed size tell a full snapshot from a stub, and neither can be read from the
 * frame once it is encrypted.
 * @param stage Push stage name.
 * @param family Family type the frame carries.
 * @param objects Object count in the frame.
 * @param bytes Bytes the frame added to the response, framing included.
 * @param recorded Record outcome, or a negative value when the stage has none.
 */
void push(const char* stage,
          unsigned family,
          std::size_t objects,
          std::size_t bytes,
          int recorded) noexcept;

/** Reports one subscription step that produced no frame. */
void subscription_failure(const char* step) noexcept;

/** Reports one subscription step that did not record. */
void subscription_state(const char* step) noexcept;

} // namespace sunrise::server::bap::encrypted::push::queuez_report
