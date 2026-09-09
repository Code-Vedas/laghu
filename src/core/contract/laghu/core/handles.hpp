// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/core/contract.hpp>

namespace laghu::core {

namespace internal {
struct DescriptorOperations;
class HandleTestAccess;
}  // namespace internal

class BorrowedFileHandle final {
 public:
  constexpr BorrowedFileHandle() noexcept = default;

  [[nodiscard]] constexpr bool is_valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] constexpr int native_handle() const noexcept { return descriptor_; }

 private:
  friend class FileHandle;

  explicit constexpr BorrowedFileHandle(int descriptor) noexcept : descriptor_(descriptor) {}

  int descriptor_{-1};
};

class BorrowedSocketHandle final {
 public:
  constexpr BorrowedSocketHandle() noexcept = default;

  [[nodiscard]] constexpr bool is_valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] constexpr int native_handle() const noexcept { return descriptor_; }

 private:
  friend class SocketHandle;

  explicit constexpr BorrowedSocketHandle(int descriptor) noexcept : descriptor_(descriptor) {}

  int descriptor_{-1};
};

class FileHandle final {
 public:
  static constexpr int invalid_descriptor = -1;

  constexpr FileHandle() noexcept = default;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  ~FileHandle();

  [[nodiscard]] static Result<FileHandle> adopt(int descriptor) noexcept;

  [[nodiscard]] constexpr bool is_valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] constexpr int native_handle() const noexcept { return descriptor_; }
  [[nodiscard]] constexpr BorrowedFileHandle borrow() const noexcept {
    return BorrowedFileHandle{descriptor_};
  }
  [[nodiscard]] int release() noexcept;
  [[nodiscard]] Result<void> close() noexcept;
  [[nodiscard]] Result<void> reset(FileHandle&& replacement) noexcept;

 private:
  friend class internal::HandleTestAccess;

  constexpr FileHandle(int descriptor, const internal::DescriptorOperations* operations) noexcept
      : descriptor_(descriptor), operations_(operations) {}

  void move_from(FileHandle&& other) noexcept;

  int descriptor_{invalid_descriptor};
  const internal::DescriptorOperations* operations_{nullptr};
};

class SocketHandle final {
 public:
  static constexpr int invalid_descriptor = -1;

  constexpr SocketHandle() noexcept = default;
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;
  SocketHandle(SocketHandle&& other) noexcept;
  SocketHandle& operator=(SocketHandle&& other) noexcept;
  ~SocketHandle();

  [[nodiscard]] static Result<SocketHandle> adopt(int descriptor) noexcept;

  [[nodiscard]] constexpr bool is_valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] constexpr int native_handle() const noexcept { return descriptor_; }
  [[nodiscard]] constexpr BorrowedSocketHandle borrow() const noexcept {
    return BorrowedSocketHandle{descriptor_};
  }
  [[nodiscard]] int release() noexcept;
  [[nodiscard]] Result<void> close() noexcept;
  [[nodiscard]] Result<void> reset(SocketHandle&& replacement) noexcept;

 private:
  friend class internal::HandleTestAccess;

  constexpr SocketHandle(int descriptor, const internal::DescriptorOperations* operations) noexcept
      : descriptor_(descriptor), operations_(operations) {}

  void move_from(SocketHandle&& other) noexcept;

  int descriptor_{invalid_descriptor};
  const internal::DescriptorOperations* operations_{nullptr};
};

static_assert(!std::is_copy_constructible_v<FileHandle>);
static_assert(!std::is_copy_assignable_v<FileHandle>);
static_assert(std::is_nothrow_move_constructible_v<FileHandle>);
static_assert(std::is_nothrow_move_assignable_v<FileHandle>);
static_assert(!std::is_copy_constructible_v<SocketHandle>);
static_assert(!std::is_copy_assignable_v<SocketHandle>);
static_assert(std::is_nothrow_move_constructible_v<SocketHandle>);
static_assert(std::is_nothrow_move_assignable_v<SocketHandle>);
static_assert(!std::is_convertible_v<FileHandle, SocketHandle>);
static_assert(!std::is_convertible_v<SocketHandle, FileHandle>);

}  // namespace laghu::core
