#pragma once

#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../thread.h"

namespace dxvk {

  enum class LogLevel : uint32_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    None  = 5,
  };

#ifdef _WIN32
  using PFN_wineLogOutput = int (__cdecl *)(const char *);
#endif

  /**
   * \brief Receiver of log lines in place of the log file
   *
   * \c fn gets one line at a time, without prefix or newline, on the
   * thread that logs and with the logger's lock held, so it must not
   * log itself. \c context identifies the sink for removal.
   */
  struct LogSink {
    void* context;
    void (*fn)(void* context, LogLevel level, const char* line);
  };

  /**
   * \brief Logger
   * 
   * Logger for one DLL. Creates a text file and
   * writes all log messages to that file.
   */
  class Logger {
    
  public:
    
    Logger(const std::string& file_name);

    /**
     * \brief Logger that creates a file only when DXVK_LOG_PATH is set
     *
     * For a DLL loaded as a system driver into every process: the
     * default file in the working directory would land anywhere.
     */
    Logger(const std::string& file_name, bool requireLogPath);

    ~Logger();
    
    static void trace(const std::string& message);
    static void debug(const std::string& message);
    static void info (const std::string& message);
    static void warn (const std::string& message);
    static void err  (const std::string& message);
    static void log  (LogLevel level, const std::string& message);

    /**
     * \brief Registers a log sink
     *
     * While any sink is registered, lines go to the one registered
     * first and nowhere else, not to the file or the debug output.
     * \param [in] sink The sink
     */
    static void addSink(const LogSink& sink);

    /**
     * \brief Removes a log sink
     *
     * Once this returns, the sink receives no further lines.
     * \param [in] context The sink's context
     */
    static void removeSink(void* context);

    static LogLevel logLevel() {
      return s_instance.m_minLevel;
    }
    
  private:
    
    static Logger     s_instance;
    
    const LogLevel    m_minLevel;
    const std::string m_fileName;
    const bool        m_requireLogPath = false;

    dxvk::mutex       m_mutex;
    std::ofstream     m_fileStream;
    std::vector<LogSink> m_sinks;

    bool              m_initialized = false;
#ifdef _WIN32
    PFN_wineLogOutput m_wineLogOutput = nullptr;
#endif

    void emitMsg(LogLevel level, const std::string& message);
    
    std::string getFileName(
      const std::string& base);

    static LogLevel getMinLogLevel();

  };
  
}
