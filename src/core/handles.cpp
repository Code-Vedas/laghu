// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>

#include <unistd.h>

#include <laghu/core/handles.hpp>

#include <laghu/core/internal/descriptor_operations.hpp>

namespace laghu::core {
namespace {

[[nodiscard]] int close_descriptor(void*, int descriptor) noexcept { return ::close(descriptor); }

constexpr internal::DescriptorOperations default_operations{nullptr, close_descriptor};

[[nodiscard]] Error invalid_descriptor_error() noexcept {
  return Error{ErrorDomain::core, ErrorCode::invalid_input, EINVAL,
               "descriptor adoption requires a non-negative descriptor"};
}

[[nodiscard]] Result<void> close_descriptor_once(
    int descriptor, const internal::DescriptorOperations* operations) noexcept {
  if (operations == nullptr || operations->close == nullptr) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                 "descriptor has no close operation"}};
  }
  if (operations->close(operations->context, descriptor) == 0) {
    return {};
  }
  return std::unexpected{Error::from_errno(errno, "descriptor close failed")};
}

}  // namespace

namespace internal {

const DescriptorOperations& default_descriptor_operations() noexcept { return default_operations; }

FileHandle HandleTestAccess::adopt_file(int descriptor,
                                        const DescriptorOperations& operations) noexcept {
  return FileHandle{descriptor, &operations};
}

SocketHandle HandleTestAccess::adopt_socket(int descriptor,
                                            const DescriptorOperations& operations) noexcept {
  return SocketHandle{descriptor, &operations};
}

}  // namespace internal

FileHandle::FileHandle(FileHandle&& other) noexcept { move_from(static_cast<FileHandle&&>(other)); }

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    move_from(static_cast<FileHandle&&>(other));
  }
  return *this;
}

FileHandle::~FileHandle() { static_cast<void>(close()); }

Result<FileHandle> FileHandle::adopt(int descriptor) noexcept {
  if (descriptor < 0) {
    return std::unexpected{invalid_descriptor_error()};
  }
  return FileHandle{descriptor, &internal::default_descriptor_operations()};
}

int FileHandle::release() noexcept {
  const int released = descriptor_;
  descriptor_ = invalid_descriptor;
  operations_ = nullptr;
  return released;
}

Result<void> FileHandle::close() noexcept {
  if (!is_valid()) {
    return {};
  }
  const internal::DescriptorOperations* operations = operations_;
  const int descriptor = release();
  return close_descriptor_once(descriptor, operations);
}

Result<void> FileHandle::reset(FileHandle&& replacement) noexcept {
  if (this == &replacement) {
    return {};
  }
  if (is_valid() && replacement.is_valid() && descriptor_ == replacement.descriptor_) {
    static_cast<void>(replacement.release());
    return {};
  }
  const Result<void> old_close = close();
  move_from(static_cast<FileHandle&&>(replacement));
  return old_close;
}

void FileHandle::move_from(FileHandle&& other) noexcept {
  const internal::DescriptorOperations* operations = other.operations_;
  descriptor_ = other.release();
  operations_ = operations;
  if (!is_valid()) {
    operations_ = nullptr;
  }
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept {
  move_from(static_cast<SocketHandle&&>(other));
}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    move_from(static_cast<SocketHandle&&>(other));
  }
  return *this;
}

SocketHandle::~SocketHandle() { static_cast<void>(close()); }

Result<SocketHandle> SocketHandle::adopt(int descriptor) noexcept {
  if (descriptor < 0) {
    return std::unexpected{invalid_descriptor_error()};
  }
  return SocketHandle{descriptor, &internal::default_descriptor_operations()};
}

int SocketHandle::release() noexcept {
  const int released = descriptor_;
  descriptor_ = invalid_descriptor;
  operations_ = nullptr;
  return released;
}

Result<void> SocketHandle::close() noexcept {
  if (!is_valid()) {
    return {};
  }
  const internal::DescriptorOperations* operations = operations_;
  const int descriptor = release();
  return close_descriptor_once(descriptor, operations);
}

Result<void> SocketHandle::reset(SocketHandle&& replacement) noexcept {
  if (this == &replacement) {
    return {};
  }
  if (is_valid() && replacement.is_valid() && descriptor_ == replacement.descriptor_) {
    static_cast<void>(replacement.release());
    return {};
  }
  const Result<void> old_close = close();
  move_from(static_cast<SocketHandle&&>(replacement));
  return old_close;
}

void SocketHandle::move_from(SocketHandle&& other) noexcept {
  const internal::DescriptorOperations* operations = other.operations_;
  descriptor_ = other.release();
  operations_ = operations;
  if (!is_valid()) {
    operations_ = nullptr;
  }
}

}  // namespace laghu::core
