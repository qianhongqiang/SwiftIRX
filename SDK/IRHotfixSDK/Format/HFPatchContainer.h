#ifndef IRHotfixSDK_HFPatchContainer_h
#define IRHotfixSDK_HFPatchContainer_h

#include "HFIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace irhotfix::container {

inline constexpr std::uint16_t kVersion = 1;

enum class SectionType : std::uint32_t {
  Metadata = 1,
  Constants = 2,
  HostImports = 3,
  Functions = 4,
  DebugInformation = 5,
  Signature = 6,
};

// Encodes a verified HFIR package as a deterministic .hfpatch byte stream.
bool encode(const hfir::Package &package, std::vector<std::uint8_t> &output,
            std::string &error);

// Performs structural, integrity, bounds and HFIR semantic validation.
bool decode(const std::vector<std::uint8_t> &input, hfir::Package &package,
            std::string &error);

bool readFile(const std::string &path, std::vector<std::uint8_t> &output,
              std::string &error);
bool writeFile(const std::string &path,
               const std::vector<std::uint8_t> &bytes, std::string &error);

// Human-readable deterministic output used for inspection and build checks.
std::string dump(const hfir::Package &package);

} // namespace irhotfix::container

#endif /* IRHotfixSDK_HFPatchContainer_h */
