// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/Logger.h>
#include <Interpreters/Settings.h>
#include <common/logger_useful.h>

namespace DB
{
namespace ErrorCodes
{
extern const int UNKNOWN_SETTING;
extern const int THERE_IS_NO_PROFILE;
extern const int NO_ELEMENTS_IN_CONFIG;
} // namespace ErrorCodes


/// Set the configuration by name.
void Settings::set(const String & name, const Field & value)
{
#define TRY_SET(TYPE, NAME, DEFAULT, DESCRIPTION) else if (name == #NAME)(NAME).set(value);

    if (false) {} // NOLINT(readability-simplify-boolean-expr)
    APPLY_FOR_SETTINGS(TRY_SET)
    else
    {
        LOG_ERROR(Logger::get(), "Unknown setting, name={} value={}", name, value.toString());
    }

#undef TRY_SET
}

/// Set the configuration by name. Read the binary serialized value from the buffer (for interserver interaction).
void Settings::set(const String & name, ReadBuffer & buf)
{
#define TRY_SET(TYPE, NAME, DEFAULT, DESCRIPTION) else if (name == #NAME)(NAME).set(buf);

    if (false) {} // NOLINT(readability-simplify-boolean-expr)
    APPLY_FOR_SETTINGS(TRY_SET)
    else
    {
        LOG_ERROR(Logger::get(), "Unknown setting, name={}", name);
    }

#undef TRY_SET
}

/// Skip the binary-serialized value from the buffer.
void Settings::ignore(const String & name, ReadBuffer & buf)
{
#define TRY_IGNORE(TYPE, NAME, DEFAULT, DESCRIPTION) else if (name == #NAME) decltype(NAME)(DEFAULT).set(buf);

    if (false) {} // NOLINT(readability-simplify-boolean-expr)
    APPLY_FOR_SETTINGS(TRY_IGNORE)
    else
    {
        LOG_ERROR(Logger::get(), "Unknown setting, name={}", name);
    }

#undef TRY_IGNORE
}

/** Set the setting by name. Read the value in text form from a string (for example, from a config, or from a URL parameter).
    */
void Settings::set(const String & name, const String & value)
{
#define TRY_SET(TYPE, NAME, DEFAULT, DESCRIPTION) else if (name == #NAME)(NAME).set(value);

    if (false) {} // NOLINT(readability-simplify-boolean-expr)
    APPLY_FOR_SETTINGS(TRY_SET)
    else
    {
        LOG_ERROR(Logger::get(), "Unknown setting, name={}", name);
    }

#undef TRY_SET
}

String Settings::get(const String & name) const
{
#define GET(TYPE, NAME, DEFAULT, DESCRIPTION) else if (name == #NAME) return (NAME).toString();

    if (false) {} // NOLINT(readability-simplify-boolean-expr)
    APPLY_FOR_SETTINGS(GET)
    else
    {
        throw Exception("Unknown setting " + name, ErrorCodes::UNKNOWN_SETTING);
    }

#undef GET
}

bool Settings::tryGet(const String & name, String & value) const
{
#define TRY_GET(TYPE, NAME, DEFAULT, DESCRIPTION) \
    else if (name == #NAME)                       \
    {                                             \
        value = (NAME).toString();                \
        return true;                              \
    }

    if (false) {} // NOLINT(readability-simplify-boolean-expr)
    APPLY_FOR_SETTINGS(TRY_GET)
    else
    {
        return false;
    }

#undef TRY_GET
}

/** Set the settings from the profile (in the server configuration, many settings can be listed in one profile).
    * The profile can also be set using the `set` functions, like the `profile` setting.
    */
void Settings::setProfile(const String & profile_name, Poco::Util::AbstractConfiguration & config)
{
    String elem = "profiles." + profile_name;
    if (!config.has(elem))
    {
        // Allow no "profiles.*" configurations for TiFlash
        // throw Exception("There is no profile '" + profile_name + "' in configuration file.", ErrorCodes::THERE_IS_NO_PROFILE);
        return;
    }

    Poco::Util::AbstractConfiguration::Keys config_keys;
    config.keys(elem, config_keys);

    for (const std::string & key : config_keys)
    {
        if (key == "profile") /// Inheritance of one profile from another.
            setProfile(config.getString(elem + "." + key), config);
        else
            set(key, config.getString(elem + "." + key));
    }
}

void Settings::loadSettingsFromConfig(const String & path, const Poco::Util::AbstractConfiguration & config)
{
    if (!config.has(path))
        throw Exception("There is no path '" + path + "' in configuration file.", ErrorCodes::NO_ELEMENTS_IN_CONFIG);

    Poco::Util::AbstractConfiguration::Keys config_keys;
    config.keys(path, config_keys);

    for (const std::string & key : config_keys)
    {
        set(key, config.getString(path + "." + key));
    }
}

/// Read the settings from the buffer. They are written as a set of name-value pairs that go successively, ending with an empty `name`.
/// If the `check_readonly` flag is set, `readonly` is set in the preferences, but some changes have occurred - throw an exception.
void Settings::deserialize(ReadBuffer & buf)
{
    auto before_readonly = readonly;

    while (true)
    {
        String name;
        readBinary(name, buf);

        /// An empty string is the marker for the end of the settings.
        if (name.empty())
            break;

        /// If readonly = 2, then you can change the settings, except for the readonly setting.
        if (before_readonly == 0 || (before_readonly == 2 && name != "readonly"))
            set(name, buf);
        else
            ignore(name, buf);
    }
}

/// Record the changed settings to the buffer. (For example, to send to a remote server.)
void Settings::serialize(WriteBuffer & buf) const
{
#define WRITE(TYPE, NAME, DEFAULT, DESCRIPTION) \
    if ((NAME).changed)                         \
    {                                           \
        writeStringBinary(#NAME, buf);          \
        (NAME).write(buf);                      \
    }

    APPLY_FOR_SETTINGS(WRITE)

    /// An empty string is a marker for the end of the settings.
    writeStringBinary("", buf);

#undef WRITE
}

String Settings::toString() const
{
    FmtBuffer buf;
#define WRITE(TYPE, NAME, DEFAULT, DESCRIPTION)                                   \
    if ((NAME).changed)                                                           \
    {                                                                             \
        buf.fmtAppend("{}={}(default {}), ", #NAME, (NAME).toString(), #DEFAULT); \
    }

    APPLY_FOR_SETTINGS(WRITE)

#undef WRITE
    if (buf.size() > 2)
        buf.resize(buf.size() - 2);
    return buf.toString();
}

} // namespace DB
