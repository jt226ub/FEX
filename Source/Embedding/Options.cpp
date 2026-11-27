// SPDX-License-Identifier: MIT
//
// Checks the options an embedder passes and turns them into a configuration layer.
//
// The core's own configuration loaders never reject a value: a number that does not parse
// becomes 0, and a value of the wrong kind can stop the process later, when it is first
// read. The library checks each option when the context is created instead, so a bad
// option is reported as FEXMAC_ERROR_BAD_CONFIG before anything process-wide changes.
#include "Internal.h"

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/fmt.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

namespace fexmac {
namespace {
  enum class Kind { Bool, UInt8, Int32, UInt32, UInt64, String, StringArray, StringEnum };

  struct OptionInfo {
    const char* EnvName;
    const char* FileName;
    FEXCore::Config::ConfigOption Option;
    Kind Type;
  };

#define OPT_BASE(type, group, enum, json, default)
#define OPT_BOOL(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::Bool},
#define OPT_UINT8(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::UInt8},
#define OPT_INT32(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::Int32},
#define OPT_UINT32(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::UInt32},
#define OPT_UINT64(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::UInt64},
#define OPT_STR(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::String},
#define OPT_STRARRAY(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::StringArray},
#define OPT_STRENUM(group, enum, json, default) {#enum, #json, FEXCore::Config::CONFIG_##enum, Kind::StringEnum},
  constexpr OptionInfo Options[] = {
#include <FEXCore/Config/ConfigValues.inl>
  };

  const OptionInfo* FindOption(std::string_view Name) {
    for (const auto& Info : Options) {
      if (Name == Info.EnvName || Name == Info.FileName) {
        return &Info;
      }
    }
    return nullptr;
  }

  template<typename T>
  bool ParseInteger(std::string_view Value, T Min, T Max) {
    if (Value.empty()) {
      return false;
    }
    const fextl::string Copy {Value};
    char* End {};
    errno = 0;
    if constexpr (std::numeric_limits<T>::is_signed) {
      const long long Result = std::strtoll(Copy.c_str(), &End, 0);
      return errno == 0 && *End == '\0' && Result >= Min && Result <= Max;
    } else {
      if (Copy[0] == '-') {
        return false;
      }
      const unsigned long long Result = std::strtoull(Copy.c_str(), &End, 0);
      return errno == 0 && *End == '\0' && Result >= Min && Result <= Max;
    }
  }

  // Applies the named-value conversions the core's environment loader does (for example
  // SMCCHECKS=full), keyed by the environment-variable name.
  fextl::string ConvertNamedValue(const OptionInfo& Info, std::string_view Value_View) {
    const fextl::string KeyString = fextl::fmt::format("FEX_{}", Info.EnvName);
    const std::string_view Key {KeyString};
    std::optional<fextl::string> Value;
#define ENVLOADER
#include <FEXCore/Config/ConfigOptions.inl>
    return Value ? *Value : fextl::string {Value_View};
  }

  void ReportBadOption(const char* Name, const char* Value, const char* Why) {
    LogMan::Msg::EFmt("fexmac: option {}={}: {}", Name ? Name : "(null)", Value ? Value : "(null)", Why);
  }

  class OptionsLayer final : public FEXCore::Config::Layer {
  public:
    explicit OptionsLayer(ParsedOptions _Values)
      : FEXCore::Config::Layer(FEXCore::Config::LayerType::LAYER_ENVIRONMENT)
      , Values {std::move(_Values)} {}

    void Load() override {
      for (const auto& [Option, Value] : Values) {
        const auto* Info = [&]() -> const OptionInfo* {
          for (const auto& It : Options) {
            if (It.Option == Option) {
              return &It;
            }
          }
          return nullptr;
        }();
        if (Info && Info->Type == Kind::StringArray) {
          AppendStrArrayValue(Option, Value);
        } else {
          Set(Option, Value);
        }
      }
    }

  private:
    ParsedOptions Values;
  };
} // namespace

fexmac_result ParseOptions(const fexmac_config& Config, ParsedOptions* Out) {
  if (Config.option_count && !Config.options) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }

  for (size_t i = 0; i < Config.option_count; ++i) {
    const char* Name = Config.options[i].name;
    const char* Value = Config.options[i].value;
    if (!Name || !Value) {
      ReportBadOption(Name, Value, "name and value must both be set");
      return FEXMAC_ERROR_BAD_CONFIG;
    }

    const OptionInfo* Info = FindOption(Name);
    if (!Info) {
      ReportBadOption(Name, Value, "unknown option");
      return FEXMAC_ERROR_BAD_CONFIG;
    }

    fextl::string Converted = ConvertNamedValue(*Info, Value);
    bool Valid = true;
    switch (Info->Type) {
    case Kind::Bool: Valid = Converted == "0" || Converted == "1"; break;
    case Kind::UInt8: Valid = ParseInteger<uint64_t>(Converted, 0, std::numeric_limits<uint8_t>::max()); break;
    case Kind::Int32:
      Valid = ParseInteger<int64_t>(Converted, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
      break;
    case Kind::UInt32: Valid = ParseInteger<uint64_t>(Converted, 0, std::numeric_limits<uint32_t>::max()); break;
    case Kind::UInt64:
    case Kind::StringEnum: Valid = ParseInteger<uint64_t>(Converted, 0, std::numeric_limits<uint64_t>::max()); break;
    case Kind::String:
    case Kind::StringArray: break;
    }

    // The environment loader maps an unknown self-modifying-code mode to "none"
    // silently; here it is an error.
    if (Valid && Info->Option == FEXCore::Config::CONFIG_SMCCHECKS) {
      const std::string_view Raw {Value};
      const bool Named = Raw == "none" || Raw == "mtrack" || Raw == "full";
      Valid = Named || (ParseInteger<uint64_t>(Raw, 0, 2));
    }
    if (!Valid) {
      ReportBadOption(Name, Value, "value does not parse for this option");
      return FEXMAC_ERROR_BAD_CONFIG;
    }

    // Only 64-bit guests: the first 4GB of a macOS process cannot hold guest memory.
    if (Info->Option == FEXCore::Config::CONFIG_IS64BIT_MODE && Converted != "1") {
      ReportBadOption(Name, Value, "only 64-bit guests are supported on macOS");
      return FEXMAC_ERROR_BAD_CONFIG;
    }

    Out->emplace_back(Info->Option, std::move(Converted));
  }
  return FEXMAC_OK;
}

fextl::unique_ptr<FEXCore::Config::Layer> CreateOptionsLayer(ParsedOptions Options) {
  return fextl::make_unique<OptionsLayer>(std::move(Options));
}
} // namespace fexmac

extern "C" int fexmac_option_known(const char* name) {
  return name && fexmac::FindOption(name) ? 1 : 0;
}
