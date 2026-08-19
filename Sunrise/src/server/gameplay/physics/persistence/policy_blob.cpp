#include <algorithm>

#include "checkpoint.h"

namespace sunrise::server::gameplay::physics::persistence {

/** Starts a bounded policy-state write at offset zero. */
PolicyBlobWriter::PolicyBlobWriter(PolicyStateBlob& blob) noexcept : blob_(&blob) {
    blob = {};
}

/** Appends one policy-owned byte range atomically. */
bool PolicyBlobWriter::write(std::span<const std::byte> bytes) noexcept {
    if (blob_ == nullptr || bytes.size() > blob_->bytes.size() - blob_->size) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), blob_->bytes.data() + blob_->size);
    blob_->size += bytes.size();
    return true;
}

/** Starts a bounded policy-state read at offset zero. */
PolicyBlobReader::PolicyBlobReader(const PolicyStateBlob& blob) noexcept : blob_(&blob) {}

/** Reads one exact policy-owned byte range atomically. */
bool PolicyBlobReader::read(std::span<std::byte> bytes) noexcept {
    if (blob_ == nullptr || blob_->size > blob_->bytes.size() || offset_ > blob_->size
        || bytes.size() > blob_->size - offset_) {
        return false;
    }
    std::copy(
        blob_->bytes.data() + offset_, blob_->bytes.data() + offset_ + bytes.size(), bytes.begin());
    offset_ += bytes.size();
    return true;
}

/** Reports whether policy load consumed the entire retained blob. */
bool PolicyBlobReader::consumed_all() const noexcept {
    return blob_ != nullptr && offset_ == blob_->size;
}

} // namespace sunrise::server::gameplay::physics::persistence
