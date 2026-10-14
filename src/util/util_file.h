#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "util_flags.h"
#include "util_likely.h"

#include "./rc/util_rc_ptr.h"

namespace dxvk::util {

  /**
   * \brief File flags
   */
  enum class FileFlag : uint32_t {
    AllowRead       = 0,
    AllowWrite      = 1,
    Truncate        = 2,
    Exclusive       = 3,
    /// Opens the file and creates it when it does not exist, keeping the content of an existing
    /// one. Needed when the content may only be judged with the file lock held, which needs the
    /// file open: a Truncate open would discard what another process is using.
    Create          = 4,
  };

  using FileFlags = Flags<FileFlag>;


  /**
   * \brief Platform-specific file interface
   */
  class FileIface {

  public:

    virtual ~FileIface();

    virtual bool read(size_t offset, size_t size, void* data) = 0;

    virtual bool write(size_t offset, size_t size, const void* data) = 0;

    virtual bool append(size_t size, const void* data) = 0;

    virtual size_t size() = 0;

    virtual bool status() const = 0;

    virtual bool flush() = 0;

    /**
     * \brief Shortens the file to the given size
     *
     * Everything beyond \c size is discarded. Used to drop the partial
     * record that a process killed inside an append leaves behind.
     * \param [in] size New file size, in bytes
     * \returns \c true on success
     */
    virtual bool truncate(size_t size) = 0;

    /**
     * \brief Takes the file lock
     *
     * Serializes the processes that share this file. On Windows this is a
     * byte-range lock and it holds across processes; elsewhere it holds
     * between the threads of this process only. Windows acquisition retries
     * for at most 250 ms. Nested calls on one file object are not allowed.
     * \param [in] exclusive \c true for a writer, \c false for a reader
     * \returns \c true if the lock is held
     */
    virtual bool lock(bool exclusive) = 0;

    virtual void unlock() = 0;

    force_inline void incRef() {
      m_refCount.fetch_add(1u);
    }

    force_inline void decRef() {
      if (m_refCount.fetch_sub(1u) == 1u)
        delete this;
    }

  private:

    std::atomic<uint32_t> m_refCount = { 0u };

  };


  /**
   * \brief Generic file interface
   *
   * Provides a basic API for exclusive file I/O, which is
   * not (yet) available in the cpp standard library.
   * Note that this file API is not thread-safe.
   */
  class File {

  public:

    File();

    File(const std::string& path, FileFlags flags);

    File(File&& other);

    File& operator = (File&& other);

    ~File();

    bool open(const std::string& path, FileFlags flags);

    bool read(size_t offset, size_t size, void* data);

    bool write(size_t offset, size_t size, const void* data);

    bool append(size_t size, const void* data);

    size_t size();

    bool flush();

    bool truncate(size_t size);

    bool lock(bool exclusive);

    void unlock();

    explicit operator bool () const;

  private:

    Rc<FileIface> m_impl;

  };

}
