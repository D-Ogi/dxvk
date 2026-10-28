#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxvk {

  /**
   * \brief Tri-state
   * 
   * Used to conditionally override
   * booleans if desired.
   */
  enum class Tristate : int32_t {
    Auto  = -1,
    False =  0,
    True  =  1,
  };

  /**
   * \brief Config option set
   * 
   * Stores configuration options
   * as a set of key-value pairs.
   */
  class Config {
    using OptionMap = std::unordered_map<std::string, std::string>;
  public:

    Config();
    Config(OptionMap&& options);
    ~Config();

    /**
     * \brief Merges two configuration sets
     * 
     * Options specified in this config object will
     * not be overridden if they are specified in
     * the second config object.
     * \param [in] other Config set to merge.
     */
    void merge(const Config& other);

    /**
     * \brief Sets an option
     * 
     * \param [in] key Option name
     * \param [in] value Option value
     */
    void setOption(
      const std::string& key,
      const std::string& value);

    /**
     * \brief Parses an option value
     *
     * Retrieves the option value as a string, and then
     * tries to convert that string to the given type.
     * If parsing the string fails because it is either
     * invalid or if the option is not defined, this
     * method will return a fallback value.
     * 
     * Currently, this supports the types \c bool,
     * \c int32_t, and \c std::string.
     * \tparam T Return value type
     * \param [in] option Option name
     * \param [in] fallback Fallback value
     * \returns Parsed option value
     * \returns The parsed option value
     */
    template<typename T>
    T getOption(const char* option, T fallback = T()) const {
      const std::string& value = getOptionValue(option);

      T result = fallback;
      parseOptionValue(value, result);
      return result;
    }

    /**
     * \brief Logs option values
     * 
     * Prints the effective configuration
     * to the log for debugging purposes. 
     */
    void logOptions() const;

    /**
     * \brief Options with a name prefix
     *
     * \param [in] prefix Start of the option names
     * \returns The name and value of every option whose
     *    name starts with \c prefix, sorted by name
     */
    std::vector<std::pair<std::string, std::string>> getOptions(
      const std::string& prefix) const;

    /**
     * \brief Retrieves default options for an app
     * 
     * \param [in] appName Name of the application
     * \returns Default options for the application
     */
    static Config getAppConfig(const std::string& appName);

    /**
     * \brief Retrieves user configuration
     * 
     * Reads options from the configuration file,
     * if it can be found, or an empty option set.
     * \returns User-defined configuration options
     */
    static Config getUserConfig();

    /**
     * \brief Retrieves the machine-wide profile configuration
     *
     * amdgpu-wddm: the options of the [executable] sections for
     * this executable in the dxvk.conf of the machine profile
     * directory (config_profile.cpp), or an empty option set.
     * \returns Options of the profile file
     */
    static Config getProfileConfig();

    /**
     * \brief Retrieves the configuration of this process
     *
     * amdgpu-wddm: the user configuration, then the machine
     * profile file, then the built-in profiles. DxvkInstance
     * and the engine export of the app options use it, so
     * both read the same options.
     * \param [in] exePath Path of the executable
     * \returns Merged configuration
     */
    static Config getProcessConfig(const std::string& exePath);

    /**
     * \brief Parses the text of a profile file
     *
     * Checks the whole text first. On an error, nothing is
     * set and the error names the line.
     * \param [in] text The file
     * \param [in] exeName The executable name of the sections that apply
     * \param [out] config Gets the options of those sections
     * \param [out] error The reason of a refusal
     * \param [out] applied Optional: the line and text of each option set
     * \returns \c true when the file is valid
     */
    static bool parseProfileConfig(
      const std::string&                          text,
      const std::string&                          exeName,
            Config&                               config,
            std::string&                          error,
            std::vector<std::pair<uint32_t, std::string>>* applied);

#ifdef _WIN32
    /**
     * \brief Reads the profile file of a profile directory
     *
     * For the host tests: \c trustAll skips the owner and
     * access checks of the directory and the file.
     */
    static Config getProfileConfigFrom(
      const std::wstring&                         root,
      const std::string&                          exeName,
            bool                                  trustAll);

    /**
     * \brief The access rule of the profile files
     *
     * \c true when SYSTEM, Administrators or TrustedInstaller
     * owns the object and no other account may change it.
     */
    static bool isTrustedProfileDescriptor(
            void*                                 securityDescriptor,
            std::string&                          why);
#endif

    static std::string toLower(std::string str);

  private:

    OptionMap m_options;

    std::string getOptionValue(
      const char*         option) const;

    static bool parseOptionValue(
      const std::string&  value,
            std::string&  result);

    static bool parseOptionValue(
      const std::string&  value,
            bool&         result);

    static bool parseOptionValue(
      const std::string&  value,
            int32_t&      result);
    
    static bool parseOptionValue(
      const std::string&  value,
            float&        result);

    static bool parseOptionValue(
      const std::string&  value,
            Tristate&     result);

    template<typename I, typename V>
    static bool parseStringOption(
            std::string   str,
            I             begin,
            I             end,
            V&            value);

  };


  /**
   * \brief Applies tristate option
   * 
   * Overrides the given value if \c state is
   * \c True or \c False, and leaves it intact
   * otherwise.
   * \param [out] option The value to override
   * \param [in] state Tristate to apply
   */
  inline void applyTristate(bool& option, Tristate state) {
    option &= state != Tristate::False;
    option |= state == Tristate::True;
  }

}
