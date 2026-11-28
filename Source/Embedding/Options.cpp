// SPDX-License-Identifier: MIT
//
// Checks the options an embedder passes and turns them into a configuration layer.
//
// Only options on the list below are accepted. The core has well over a hundred options;
// many belong to FEX's Linux front end, which macOS does not build, and some stop the
// process when set without it, so an option is listed only once its effect on this port
// is known. The code cache is switched on through fexmac_config, not DISKCACHE. Each listed option has its own value rule; the core's own loaders never reject a
// value (a number that does not parse becomes 0), so nothing is left to them.
//
// To add an option: put it in SupportedOptions with the narrowest rule that fits, and
// describe it in docs/macOS-embedding-interface.md ("Options").
#include "Internal.h"

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/fmt.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace fexmac {
namespace {
  enum class Rule {
    Bool,       // 0 or 1
    Positive32, // 1 to 2^31-1
    Positive64, // 1 to 2^63-1
    SMCChecks,  // none, mtrack, full (or 0, 1, 2)
    HostFeatures,
    OnlyOne, // "1": IS64BIT_MODE, which may only confirm 64-bit mode
  };

  struct OptionInfo {
    const char* EnvName;
    const char* FileName;
    FEXCore::Config::ConfigOption Option;
    Rule Check;
  };

#define OPTION(Enum, File, Check) {#Enum, #File, FEXCore::Config::CONFIG_##Enum, Rule::Check}
  constexpr OptionInfo SupportedOptions[] = {
    // Translation.
    OPTION(MULTIBLOCK, Multiblock, Bool),
    OPTION(MAXINST, MaxInst, Positive32),
    // x86 memory-ordering emulation (macOS has no hardware TSO mode for ordinary
    // processes, so this is software emulation).
    OPTION(TSOENABLED, TSOEnabled, Bool),
    OPTION(VECTORTSOENABLED, VectorTSOEnabled, Bool),
    OPTION(MEMCPYSETTSOENABLED, MemcpySetTSOEnabled, Bool),
    OPTION(HALFBARRIERTSOENABLED, HalfBarrierTSOEnabled, Bool),
    // Experimental cheaper forms of the vector ordering (off by default; effective only
    // with VECTORTSOENABLED): vector accesses as general-register acquire/release
    // accesses (a 16-byte access becomes two 8-byte ones), and merging of a vector load's
    // barrier with the next vector store's. See docs/macOS-port-status.md.
    OPTION(VECTORTSOSPLIT, VectorTSOSplit, Bool),
    OPTION(TSOBARRIERMERGE, TSOBarrierMerge, Bool),
    // Self-modifying code. "full" validates every block; "mtrack" needs page-protection
    // tracking, which package D adds; until then it detects nothing and the embedder
    // calls fexmac_invalidate. "none" detects nothing.
    OPTION(SMCCHECKS, SMCChecks, SMCChecks),
    // Guest-visible behaviour.
    OPTION(X87REDUCEDPRECISION, X87ReducedPrecision, Bool),
    OPTION(HIDEHYPERVISORBIT, HideHypervisorBit, Bool),
    OPTION(HOSTFEATURES, HostFeatures, HostFeatures),
    // RDRAND and RDSEED from the host's random generator (arc4random_buf on macOS) when the
    // CPU has no FEAT_RNG, as no Apple chip so far does; the guest CPUID then offers them.
    OPTION(SOFTWARERNG, SoftwareRNG, Bool),
    // The code cache (switched on with fexmac_config's code_cache_* fields, never with
    // DISKCACHE or DISKCACHEPATH): the size limit per cache file, and validation, which
    // compiles each cached block as well and logs differences.
    OPTION(DISKCACHEMAXFILESIZE, DiskCacheMaxFileSize, Positive64),
    OPTION(DISKCACHEVALIDATION, DiskCacheValidation, Bool),
    // Diagnostics.
    OPTION(SILENTLOG, SilentLog, Bool),
    OPTION(DUMPGPRS, DumpGPRs, Bool),
    OPTION(DISABLETELEMETRY, DisableTelemetry, Bool),
    OPTION(IS64BIT_MODE, IS64BIT_MODE, OnlyOne),
  };
#undef OPTION

  const OptionInfo* FindOption(std::string_view Name) {
    for (const auto& Info : SupportedOptions) {
      if (Name == Info.EnvName || Name == Info.FileName) {
        return &Info;
      }
    }
    return nullptr;
  }

  // Decimal, or hexadecimal with 0x. A leading 0 is refused: the core's own conversion
  // reads it as octal, which no one writing "010" means.
  std::optional<uint64_t> ParseUnsigned(std::string_view Value) {
    if (Value.empty()) {
      return std::nullopt;
    }
    unsigned Base = 10;
    if (Value.size() > 2 && Value[0] == '0' && (Value[1] == 'x' || Value[1] == 'X')) {
      Base = 16;
      Value.remove_prefix(2);
    } else if (Value.size() > 1 && Value[0] == '0') {
      return std::nullopt;
    }
    uint64_t Result = 0;
    for (char C : Value) {
      unsigned Digit;
      if (C >= '0' && C <= '9') {
        Digit = C - '0';
      } else if (Base == 16 && C >= 'a' && C <= 'f') {
        Digit = C - 'a' + 10;
      } else if (Base == 16 && C >= 'A' && C <= 'F') {
        Digit = C - 'A' + 10;
      } else {
        return std::nullopt;
      }
      if (Result > (std::numeric_limits<uint64_t>::max() - Digit) / Base) {
        return std::nullopt;
      }
      Result = Result * Base + Digit;
    }
    return Result;
  }

  // Checks a value and returns the string the core's configuration should receive.
  std::optional<fextl::string> Convert(const OptionInfo& Info, std::string_view Value) {
    switch (Info.Check) {
    case Rule::Bool:
      if (Value == "0" || Value == "1") {
        return fextl::string {Value};
      }
      return std::nullopt;
    case Rule::Positive32: {
      const auto Number = ParseUnsigned(Value);
      if (!Number || *Number < 1 || *Number > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
      }
      return fextl::fmt::format("{}", *Number);
    }
    case Rule::Positive64: {
      const auto Number = ParseUnsigned(Value);
      if (!Number || *Number < 1 || *Number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
      }
      return fextl::fmt::format("{}", *Number);
    }
    case Rule::SMCChecks:
      if (Value == "none" || Value == "0") {
        return fextl::string {"0"};
      }
      if (Value == "mtrack" || Value == "1") {
        return fextl::string {"1"};
      }
      if (Value == "full" || Value == "2") {
        return fextl::string {"2"};
      }
      return std::nullopt;
    case Rule::HostFeatures: {
      // A comma-separated list of the core's names; every name must be known (the core's
      // own parser skips unknown ones with a message). Only "off" and the disable* names:
      // an enable* name forces a feature on whether or not the host has it, and translated
      // code then executes instructions the CPU lacks (enablesve dies with SIGILL on an
      // M4).
      uint64_t Mask = 0;
      std::string_view Rest = Value;
      while (true) {
        const auto Comma = Rest.find(',');
        const std::string_view Name = Rest.substr(0, Comma);
        if (Name != "off" && !Name.starts_with("disable")) {
          return std::nullopt;
        }
        bool Found = false;
        for (const auto& [EnumName, Bits] : FEXCore::Config::HostFeatures_EnumPairs) {
          if (EnumName == Name) {
            Mask |= Bits;
            Found = true;
          }
        }
        if (!Found) {
          return std::nullopt;
        }
        if (Comma == std::string_view::npos) {
          break;
        }
        Rest.remove_prefix(Comma + 1);
      }
      return fextl::fmt::format("{}", Mask);
    }
    case Rule::OnlyOne:
      if (Value == "1") {
        return fextl::string {"1"};
      }
      return std::nullopt;
    }
    return std::nullopt;
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
        Set(Option, Value);
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
      ReportBadOption(Name, Value, "not an option this library supports");
      return FEXMAC_ERROR_BAD_CONFIG;
    }
    auto Converted = Convert(*Info, Value);
    if (!Converted) {
      ReportBadOption(Name, Value, Info->Option == FEXCore::Config::CONFIG_IS64BIT_MODE ? "only 64-bit guests are supported on macOS" :
                                                                                         "value not valid for this option");
      return FEXMAC_ERROR_BAD_CONFIG;
    }
    Out->emplace_back(Info->Option, std::move(*Converted));
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
