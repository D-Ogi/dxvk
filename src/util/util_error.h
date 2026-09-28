#pragma once

#include <string>

namespace dxvk {

  /**
   * \brief DXVK error
   *
   * A generic exception class that stores a
   * message. Exceptions should be logged.
   */
  class DxvkError {

  public:

    DxvkError() { }
    DxvkError(std::string&& message)
    : m_message(std::move(message)) { }

    const std::string& message() const {
      return m_message;
    }

    /**
     * \brief Whether this is a DxvkOutOfMemoryError
     *
     * Lets a handler that catches DxvkError report
     * E_OUTOFMEMORY without a second catch clause.
     */
    bool isOutOfMemory() const {
      return m_outOfMemory;
    }

  protected:

    DxvkError(std::string&& message, bool outOfMemory)
    : m_message(std::move(message)), m_outOfMemory(outOfMemory) { }

  private:

    std::string m_message;
    bool        m_outOfMemory = false;

  };


  /**
   * \brief Out-of-memory error
   *
   * Thrown when a resource allocation that must succeed
   * finds no memory, so that API entry points can report
   * E_OUTOFMEMORY instead of dereferencing a null storage.
   * Allocations that may fail (relocation, eviction) still
   * return null.
   */
  class DxvkOutOfMemoryError : public DxvkError {

  public:

    DxvkOutOfMemoryError(std::string&& message)
    : DxvkError(std::move(message), true) { }

  };

}
