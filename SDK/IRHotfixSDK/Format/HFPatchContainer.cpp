#include "HFPatchContainer.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace irhotfix::container {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'H', 'F', 'P', 'A',
                                                'T', 'C', 'H', 0};
constexpr std::uint32_t kHeaderSize = 64;
constexpr std::uint32_t kSectionEntrySize = 32;
constexpr std::uint64_t kMaximumFileSize = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaximumElements = 1'000'000;
constexpr std::uint32_t kFlagHasDebugInformation = 1U << 0;
constexpr std::uint32_t kFlagHasSignature = 1U << 1;

struct EncodedSection {
  SectionType type;
  std::uint32_t elementCount = 0;
  std::vector<std::uint8_t> bytes;
  std::uint64_t offset = 0;
};

struct SectionEntry {
  SectionType type;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t elementCount = 0;
};

bool fail(std::string &error, const std::string &message) {
  error = message;
  return false;
}

class Writer {
public:
  void u8(std::uint8_t value) { bytes_.push_back(value); }

  void u16(std::uint16_t value) {
    for (unsigned shift = 0; shift < 16; shift += 8)
      u8(static_cast<std::uint8_t>(value >> shift));
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
      u8(static_cast<std::uint8_t>(value >> shift));
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
      u8(static_cast<std::uint8_t>(value >> shift));
  }

  void raw(const std::uint8_t *data, std::size_t size) {
    if (size == 0)
      return;
    bytes_.insert(bytes_.end(), data, data + size);
  }

  void raw(const std::vector<std::uint8_t> &bytes) {
    raw(bytes.data(), bytes.size());
  }

  void string(const std::string &value) {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
  }

  void byteVector(const std::vector<std::uint8_t> &value) {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value);
  }

  void align(std::size_t alignment) {
    while (bytes_.size() % alignment != 0)
      u8(0);
  }

  std::size_t size() const { return bytes_.size(); }
  const std::vector<std::uint8_t> &bytes() const { return bytes_; }
  std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
  std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
  Reader(const std::uint8_t *data, std::size_t size, std::string prefix)
      : data_(data), size_(size), prefix_(std::move(prefix)) {}

  bool u8(std::uint8_t &value, std::string &error) {
    if (!require(1, error))
      return false;
    value = data_[offset_++];
    return true;
  }

  bool u16(std::uint16_t &value, std::string &error) {
    std::uint64_t decoded = 0;
    if (!unsignedValue(2, decoded, error))
      return false;
    value = static_cast<std::uint16_t>(decoded);
    return true;
  }

  bool u32(std::uint32_t &value, std::string &error) {
    std::uint64_t decoded = 0;
    if (!unsignedValue(4, decoded, error))
      return false;
    value = static_cast<std::uint32_t>(decoded);
    return true;
  }

  bool u64(std::uint64_t &value, std::string &error) {
    return unsignedValue(8, value, error);
  }

  bool string(std::string &value, std::string &error) {
    std::uint32_t length = 0;
    if (!u32(length, error) || !require(length, error))
      return false;
    value.assign(reinterpret_cast<const char *>(data_ + offset_), length);
    offset_ += length;
    return true;
  }

  bool byteVector(std::vector<std::uint8_t> &value, std::string &error) {
    std::uint32_t length = 0;
    if (!u32(length, error) || !require(length, error))
      return false;
    value.assign(data_ + offset_, data_ + offset_ + length);
    offset_ += length;
    return true;
  }

  bool count(std::uint32_t &value, std::string &error) {
    if (!u32(value, error))
      return false;
    if (value > kMaximumElements)
      return fail(error, prefix_ + ": element count exceeds the format limit");
    return true;
  }

  bool finish(std::string &error) const {
    return offset_ == size_ ||
           fail(error, prefix_ + ": section contains trailing bytes");
  }

private:
  bool require(std::size_t count, std::string &error) const {
    if (count > size_ - offset_)
      return fail(error, prefix_ + ": truncated data");
    return true;
  }

  bool unsignedValue(unsigned byteCount, std::uint64_t &value,
                     std::string &error) {
    if (!require(byteCount, error))
      return false;
    value = 0;
    for (unsigned index = 0; index < byteCount; ++index)
      value |= static_cast<std::uint64_t>(data_[offset_++]) << (index * 8);
    return true;
  }

  const std::uint8_t *data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  std::string prefix_;
};

std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool checkedRange(std::uint64_t offset, std::uint64_t size,
                  std::uint64_t limit) {
  return offset <= limit && size <= limit - offset;
}

void encodeValueTypes(Writer &writer,
                      const std::vector<hfir::ValueType> &types) {
  writer.u32(static_cast<std::uint32_t>(types.size()));
  for (hfir::ValueType type : types)
    writer.u8(static_cast<std::uint8_t>(type));
}

bool decodeValueType(Reader &reader, hfir::ValueType &type,
                     std::string &error) {
  std::uint8_t raw = 0;
  if (!reader.u8(raw, error))
    return false;
  type = static_cast<hfir::ValueType>(raw);
  return true;
}

bool decodeValueTypes(Reader &reader, std::vector<hfir::ValueType> &types,
                      std::string &error) {
  std::uint32_t count = 0;
  if (!reader.count(count, error))
    return false;
  types.resize(count);
  for (hfir::ValueType &type : types) {
    if (!decodeValueType(reader, type, error))
      return false;
  }
  return true;
}

EncodedSection encodeMetadata(const hfir::Package &package) {
  Writer writer;
  writer.string(package.patchID);
  writer.u64(package.target.targetID);
  writer.u64(package.target.signatureID);
  writer.u32(package.target.entryFunction);
  return {SectionType::Metadata, 1, writer.take()};
}

EncodedSection encodeConstants(const hfir::Package &package) {
  Writer writer;
  writer.u32(static_cast<std::uint32_t>(package.constants.size()));
  for (const hfir::Constant &constant : package.constants) {
    writer.u8(static_cast<std::uint8_t>(constant.kind));
    writer.u64(constant.bits);
    writer.byteVector(constant.bytes);
  }
  return {SectionType::Constants,
          static_cast<std::uint32_t>(package.constants.size()), writer.take()};
}

EncodedSection encodeImports(const hfir::Package &package) {
  Writer writer;
  writer.u32(static_cast<std::uint32_t>(package.imports.size()));
  for (const hfir::HostImport &import : package.imports) {
    writer.u64(import.id);
    writer.u8(static_cast<std::uint8_t>(import.kind));
    writer.u8(import.hasReceiver ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(import.returnType));
    writer.u8(0);
    writer.string(import.owner);
    writer.string(import.name);
    writer.string(import.typeEncoding);
    encodeValueTypes(writer, import.parameterTypes);
  }
  return {SectionType::HostImports,
          static_cast<std::uint32_t>(package.imports.size()), writer.take()};
}

EncodedSection encodeFunctions(const hfir::Package &package) {
  Writer writer;
  writer.u32(static_cast<std::uint32_t>(package.functions.size()));
  for (const hfir::Function &function : package.functions) {
    writer.string(function.name);
    writer.u8(static_cast<std::uint8_t>(function.returnType));
    encodeValueTypes(writer, function.parameterTypes);
    encodeValueTypes(writer, function.registerTypes);
    encodeValueTypes(writer, function.localTypes);
    writer.u32(function.entryBlock);
    writer.u32(static_cast<std::uint32_t>(function.blocks.size()));
    for (const hfir::BasicBlock &block : function.blocks) {
      writer.u32(block.id);
      writer.u32(static_cast<std::uint32_t>(block.instructions.size()));
      for (const hfir::Instruction &instruction : block.instructions) {
        writer.u16(static_cast<std::uint16_t>(instruction.opcode));
        writer.u8(static_cast<std::uint8_t>(instruction.resultType));
        writer.u8(0);
        writer.u32(instruction.result);
        writer.u32(static_cast<std::uint32_t>(instruction.operands.size()));
        for (const hfir::Operand &operand : instruction.operands) {
          writer.u8(static_cast<std::uint8_t>(operand.kind));
          writer.u8(static_cast<std::uint8_t>(operand.type));
          writer.u16(0);
          writer.u32(operand.index);
        }
      }
    }
  }
  return {SectionType::Functions,
          static_cast<std::uint32_t>(package.functions.size()), writer.take()};
}

EncodedSection encodeDebug(const hfir::Package &package) {
  Writer writer;
  writer.u32(static_cast<std::uint32_t>(package.debugLocations.size()));
  for (const hfir::DebugLocation &location : package.debugLocations) {
    writer.u32(location.function);
    writer.u32(location.block);
    writer.u32(location.instruction);
    writer.u32(location.line);
    writer.u32(location.column);
    writer.string(location.file);
  }
  return {SectionType::DebugInformation,
          static_cast<std::uint32_t>(package.debugLocations.size()),
          writer.take()};
}

EncodedSection encodeSignature(const hfir::Package &package) {
  Writer writer;
  writer.string(package.signature.algorithm);
  writer.string(package.signature.keyID);
  writer.byteVector(package.signature.bytes);
  return {SectionType::Signature, 1, writer.take()};
}

bool decodeMetadata(const SectionEntry &entry,
                    const std::vector<std::uint8_t> &input,
                    hfir::Package &package, std::string &error) {
  Reader reader(input.data() + entry.offset, entry.size, "metadata section");
  return reader.string(package.patchID, error) &&
         reader.u64(package.target.targetID, error) &&
         reader.u64(package.target.signatureID, error) &&
         reader.u32(package.target.entryFunction, error) && reader.finish(error);
}

bool decodeConstants(const SectionEntry &entry,
                     const std::vector<std::uint8_t> &input,
                     hfir::Package &package, std::string &error) {
  Reader reader(input.data() + entry.offset, entry.size, "constants section");
  std::uint32_t count = 0;
  if (!reader.count(count, error))
    return false;
  if (count != entry.elementCount)
    return fail(error, "constants section count disagrees with directory");
  package.constants.resize(count);
  for (hfir::Constant &constant : package.constants) {
    std::uint8_t kind = 0;
    if (!reader.u8(kind, error) || !reader.u64(constant.bits, error) ||
        !reader.byteVector(constant.bytes, error))
      return false;
    constant.kind = static_cast<hfir::ConstantKind>(kind);
  }
  return reader.finish(error);
}

bool decodeImports(const SectionEntry &entry,
                   const std::vector<std::uint8_t> &input,
                   hfir::Package &package, std::string &error) {
  Reader reader(input.data() + entry.offset, entry.size, "host imports section");
  std::uint32_t count = 0;
  if (!reader.count(count, error))
    return false;
  if (count != entry.elementCount)
    return fail(error, "host imports section count disagrees with directory");
  package.imports.resize(count);
  for (hfir::HostImport &import : package.imports) {
    std::uint8_t kind = 0;
    std::uint8_t receiver = 0;
    std::uint8_t returnType = 0;
    std::uint8_t reserved = 0;
    if (!reader.u64(import.id, error) || !reader.u8(kind, error) ||
        !reader.u8(receiver, error) || !reader.u8(returnType, error) ||
        !reader.u8(reserved, error) || reserved != 0 || receiver > 1)
      return reserved == 0 && receiver <= 1
                 ? false
                 : fail(error, "host imports section contains invalid flags");
    import.kind = static_cast<hfir::HostImportKind>(kind);
    import.hasReceiver = receiver != 0;
    import.returnType = static_cast<hfir::ValueType>(returnType);
    if (!reader.string(import.owner, error) || !reader.string(import.name, error) ||
        !reader.string(import.typeEncoding, error) ||
        !decodeValueTypes(reader, import.parameterTypes, error))
      return false;
  }
  return reader.finish(error);
}

bool decodeFunctions(const SectionEntry &entry,
                     const std::vector<std::uint8_t> &input,
                     hfir::Package &package, std::string &error) {
  Reader reader(input.data() + entry.offset, entry.size, "functions section");
  std::uint32_t count = 0;
  if (!reader.count(count, error))
    return false;
  if (count != entry.elementCount)
    return fail(error, "functions section count disagrees with directory");
  package.functions.resize(count);
  for (hfir::Function &function : package.functions) {
    std::uint8_t returnType = 0;
    std::uint32_t blockCount = 0;
    if (!reader.string(function.name, error) ||
        !reader.u8(returnType, error) ||
        !decodeValueTypes(reader, function.parameterTypes, error) ||
        !decodeValueTypes(reader, function.registerTypes, error) ||
        !decodeValueTypes(reader, function.localTypes, error) ||
        !reader.u32(function.entryBlock, error) ||
        !reader.count(blockCount, error))
      return false;
    function.returnType = static_cast<hfir::ValueType>(returnType);
    function.blocks.resize(blockCount);
    for (hfir::BasicBlock &block : function.blocks) {
      std::uint32_t instructionCount = 0;
      if (!reader.u32(block.id, error) ||
          !reader.count(instructionCount, error))
        return false;
      block.instructions.resize(instructionCount);
      for (hfir::Instruction &instruction : block.instructions) {
        std::uint16_t opcode = 0;
        std::uint8_t resultType = 0;
        std::uint8_t reserved = 0;
        std::uint32_t operandCount = 0;
        if (!reader.u16(opcode, error) || !reader.u8(resultType, error) ||
            !reader.u8(reserved, error) || reserved != 0 ||
            !reader.u32(instruction.result, error) ||
            !reader.count(operandCount, error))
          return reserved == 0
                     ? false
                     : fail(error, "functions section has invalid reserved bits");
        instruction.opcode = static_cast<hfir::Opcode>(opcode);
        instruction.resultType = static_cast<hfir::ValueType>(resultType);
        instruction.operands.resize(operandCount);
        for (hfir::Operand &operand : instruction.operands) {
          std::uint8_t kind = 0;
          std::uint8_t type = 0;
          std::uint16_t operandReserved = 0;
          if (!reader.u8(kind, error) || !reader.u8(type, error) ||
              !reader.u16(operandReserved, error) || operandReserved != 0 ||
              !reader.u32(operand.index, error))
            return operandReserved == 0
                       ? false
                       : fail(error,
                              "functions section has invalid operand reserved bits");
          operand.kind = static_cast<hfir::OperandKind>(kind);
          operand.type = static_cast<hfir::ValueType>(type);
        }
      }
    }
  }
  return reader.finish(error);
}

bool decodeDebug(const SectionEntry &entry,
                 const std::vector<std::uint8_t> &input,
                 hfir::Package &package, std::string &error) {
  Reader reader(input.data() + entry.offset, entry.size, "debug section");
  std::uint32_t count = 0;
  if (!reader.count(count, error))
    return false;
  if (count != entry.elementCount)
    return fail(error, "debug section count disagrees with directory");
  package.debugLocations.resize(count);
  for (hfir::DebugLocation &location : package.debugLocations) {
    if (!reader.u32(location.function, error) ||
        !reader.u32(location.block, error) ||
        !reader.u32(location.instruction, error) ||
        !reader.u32(location.line, error) ||
        !reader.u32(location.column, error) ||
        !reader.string(location.file, error))
      return false;
  }
  return reader.finish(error);
}

bool decodeSignature(const SectionEntry &entry,
                     const std::vector<std::uint8_t> &input,
                     hfir::Package &package, std::string &error) {
  Reader reader(input.data() + entry.offset, entry.size, "signature section");
  return reader.string(package.signature.algorithm, error) &&
         reader.string(package.signature.keyID, error) &&
         reader.byteVector(package.signature.bytes, error) &&
         reader.finish(error);
}

std::string escapeBytes(const std::vector<std::uint8_t> &bytes) {
  std::ostringstream output;
  output << '"';
  for (std::uint8_t byte : bytes) {
    if (byte == '\\' || byte == '"')
      output << '\\' << static_cast<char>(byte);
    else if (byte >= 0x20 && byte <= 0x7e)
      output << static_cast<char>(byte);
    else
      output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<unsigned>(byte) << std::dec;
  }
  output << '"';
  return output.str();
}

std::string hex64(std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
  return output.str();
}

} // namespace

bool encode(const hfir::Package &package, std::vector<std::uint8_t> &output,
            std::string &error) {
  error.clear();
  if (!hfir::verify(package, error))
    return false;

  std::vector<EncodedSection> sections;
  sections.push_back(encodeMetadata(package));
  sections.push_back(encodeConstants(package));
  sections.push_back(encodeImports(package));
  sections.push_back(encodeFunctions(package));
  if (!package.debugLocations.empty())
    sections.push_back(encodeDebug(package));
  if (!package.signature.bytes.empty())
    sections.push_back(encodeSignature(package));

  Writer payload;
  payload.raw(kMagic.data(), kMagic.size());
  payload.u16(kVersion);
  payload.u16(hfir::kVersion);
  payload.u32(package.abiVersion);
  std::uint32_t flags = 0;
  if (!package.debugLocations.empty())
    flags |= kFlagHasDebugInformation;
  if (!package.signature.bytes.empty())
    flags |= kFlagHasSignature;
  payload.u32(flags);
  payload.u32(static_cast<std::uint32_t>(sections.size()));
  payload.u32(kHeaderSize);
  payload.u32(0);
  payload.u64(0); // file size, patched below
  payload.u64(0); // directory offset, patched below
  payload.u64(0); // payload hash, patched below
  payload.u64(0);
  if (payload.size() != kHeaderSize)
    return fail(error, "internal error: .hfpatch header layout changed");

  for (EncodedSection &section : sections) {
    payload.align(8);
    section.offset = payload.size();
    payload.raw(section.bytes);
  }
  payload.align(8);
  const std::uint64_t directoryOffset = payload.size();
  for (const EncodedSection &section : sections) {
    payload.u32(static_cast<std::uint32_t>(section.type));
    payload.u32(0);
    payload.u64(section.offset);
    payload.u64(section.bytes.size());
    payload.u32(section.elementCount);
    payload.u32(0);
  }

  output = payload.take();
  if (output.size() > kMaximumFileSize)
    return fail(error, ".hfpatch output exceeds the 256 MiB format limit");
  auto patchU64 = [&](std::size_t offset, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
      output[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
  };
  patchU64(32, output.size());
  patchU64(40, directoryOffset);
  patchU64(48, fnv1a64(output.data() + kHeaderSize,
                       output.size() - kHeaderSize));
  return true;
}

bool decode(const std::vector<std::uint8_t> &input, hfir::Package &package,
            std::string &error) {
  error.clear();
  package = {};
  if (input.size() < kHeaderSize)
    return fail(error, ".hfpatch header is truncated");
  if (input.size() > kMaximumFileSize)
    return fail(error, ".hfpatch input exceeds the 256 MiB format limit");
  if (!std::equal(kMagic.begin(), kMagic.end(), input.begin()))
    return fail(error, ".hfpatch magic is invalid");

  Reader header(input.data() + 8, kHeaderSize - 8, ".hfpatch header");
  std::uint16_t containerVersion = 0;
  std::uint16_t hfirVersion = 0;
  std::uint32_t flags = 0;
  std::uint32_t sectionCount = 0;
  std::uint32_t headerSize = 0;
  std::uint32_t reserved = 0;
  std::uint64_t fileSize = 0;
  std::uint64_t directoryOffset = 0;
  std::uint64_t expectedHash = 0;
  std::uint64_t reserved64 = 0;
  if (!header.u16(containerVersion, error) ||
      !header.u16(hfirVersion, error) ||
      !header.u32(package.abiVersion, error) || !header.u32(flags, error) ||
      !header.u32(sectionCount, error) || !header.u32(headerSize, error) ||
      !header.u32(reserved, error) || !header.u64(fileSize, error) ||
      !header.u64(directoryOffset, error) ||
      !header.u64(expectedHash, error) || !header.u64(reserved64, error) ||
      !header.finish(error))
    return false;
  if (containerVersion != kVersion || hfirVersion != hfir::kVersion)
    return fail(error, ".hfpatch uses an unsupported container or HFIR version");
  if (headerSize != kHeaderSize || reserved != 0 || reserved64 != 0)
    return fail(error, ".hfpatch header layout or reserved fields are invalid");
  if ((flags & ~(kFlagHasDebugInformation | kFlagHasSignature)) != 0)
    return fail(error, ".hfpatch contains unknown header flags");
  if (fileSize != input.size())
    return fail(error, ".hfpatch file size does not match its header");
  if (fnv1a64(input.data() + kHeaderSize, input.size() - kHeaderSize) !=
      expectedHash)
    return fail(error, ".hfpatch payload integrity hash does not match");
  if (sectionCount < 4 || sectionCount > 6)
    return fail(error, ".hfpatch section count is invalid");
  const std::uint64_t directorySize =
      static_cast<std::uint64_t>(sectionCount) * kSectionEntrySize;
  if (!checkedRange(directoryOffset, directorySize, input.size()) ||
      directoryOffset < kHeaderSize ||
      directoryOffset + directorySize != input.size())
    return fail(error, ".hfpatch section directory is out of range");

  Reader directory(input.data() + directoryOffset, directorySize,
                   ".hfpatch section directory");
  std::map<SectionType, SectionEntry> sections;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for (std::uint32_t index = 0; index < sectionCount; ++index) {
    std::uint32_t rawType = 0;
    std::uint32_t sectionFlags = 0;
    SectionEntry entry;
    std::uint32_t sectionReserved = 0;
    if (!directory.u32(rawType, error) ||
        !directory.u32(sectionFlags, error) ||
        !directory.u64(entry.offset, error) ||
        !directory.u64(entry.size, error) ||
        !directory.u32(entry.elementCount, error) ||
        !directory.u32(sectionReserved, error))
      return false;
    if (rawType < static_cast<std::uint32_t>(SectionType::Metadata) ||
        rawType > static_cast<std::uint32_t>(SectionType::Signature))
      return fail(error, ".hfpatch contains an unknown section type");
    if (sectionFlags != 0 || sectionReserved != 0)
      return fail(error, ".hfpatch section has unsupported flags");
    entry.type = static_cast<SectionType>(rawType);
    if (!sections.emplace(entry.type, entry).second)
      return fail(error, ".hfpatch contains a duplicate section");
    if (!checkedRange(entry.offset, entry.size, directoryOffset) ||
        entry.offset < kHeaderSize)
      return fail(error, ".hfpatch section payload is out of range");
    ranges.emplace_back(entry.offset, entry.offset + entry.size);
  }
  std::sort(ranges.begin(), ranges.end());
  for (std::size_t index = 1; index < ranges.size(); ++index) {
    if (ranges[index].first < ranges[index - 1].second)
      return fail(error, ".hfpatch section payloads overlap");
  }
  for (SectionType required : {SectionType::Metadata, SectionType::Constants,
                               SectionType::HostImports,
                               SectionType::Functions}) {
    if (!sections.contains(required))
      return fail(error, ".hfpatch is missing a required section");
  }
  if (sections.at(SectionType::Metadata).elementCount != 1)
    return fail(error, ".hfpatch metadata section must contain one element");
  const bool hasDebug = sections.contains(SectionType::DebugInformation);
  const bool hasSignature = sections.contains(SectionType::Signature);
  if (hasDebug != ((flags & kFlagHasDebugInformation) != 0) ||
      hasSignature != ((flags & kFlagHasSignature) != 0))
    return fail(error, ".hfpatch optional section flags do not match directory");
  if (hasSignature && sections.at(SectionType::Signature).elementCount != 1)
    return fail(error, ".hfpatch signature section must contain one element");

  if (!decodeMetadata(sections.at(SectionType::Metadata), input, package,
                      error) ||
      !decodeConstants(sections.at(SectionType::Constants), input, package,
                       error) ||
      !decodeImports(sections.at(SectionType::HostImports), input, package,
                     error) ||
      !decodeFunctions(sections.at(SectionType::Functions), input, package,
                       error))
    return false;
  if (hasDebug &&
      !decodeDebug(sections.at(SectionType::DebugInformation), input, package,
                   error))
    return false;
  if (hasSignature &&
      !decodeSignature(sections.at(SectionType::Signature), input, package,
                       error))
    return false;
  return hfir::verify(package, error);
}

bool readFile(const std::string &path, std::vector<std::uint8_t> &output,
              std::string &error) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    return fail(error, "cannot open '" + path + "' for reading");
  const std::streamoff size = stream.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumFileSize)
    return fail(error, "input file has an invalid or unsupported size");
  output.resize(static_cast<std::size_t>(size));
  stream.seekg(0);
  if (size != 0)
    stream.read(reinterpret_cast<char *>(output.data()), size);
  if (!stream)
    return fail(error, "cannot read '" + path + "'");
  return true;
}

bool writeFile(const std::string &path,
               const std::vector<std::uint8_t> &bytes, std::string &error) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
    return fail(error, "cannot open '" + path + "' for writing");
  if (!bytes.empty())
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  if (!stream)
    return fail(error, "cannot write '" + path + "'");
  return true;
}

std::string dump(const hfir::Package &package) {
  std::ostringstream output;
  output << "hfpatch container-version " << kVersion << " hfir-version "
         << hfir::kVersion << " abi-version " << package.abiVersion << '\n';
  output << "patch " << package.patchID << '\n';
  output << "target " << hex64(package.target.targetID) << " signature "
         << hex64(package.target.signatureID) << " entry @"
         << package.target.entryFunction << '\n';

  output << "constants " << package.constants.size() << '\n';
  for (std::size_t index = 0; index < package.constants.size(); ++index) {
    const hfir::Constant &constant = package.constants[index];
    output << "  #" << index << " " << hfir::constantKindName(constant.kind);
    if (constant.kind == hfir::ConstantKind::String ||
        constant.kind == hfir::ConstantKind::Bytes ||
        constant.kind == hfir::ConstantKind::Point ||
        constant.kind == hfir::ConstantKind::Size ||
        constant.kind == hfir::ConstantKind::Rect)
      output << " " << escapeBytes(constant.bytes);
    else
      output << " bits=" << hex64(constant.bits);
    output << '\n';
  }

  output << "imports " << package.imports.size() << '\n';
  for (std::size_t index = 0; index < package.imports.size(); ++index) {
    const hfir::HostImport &import = package.imports[index];
    output << "  !" << index << " " << hfir::hostImportKindName(import.kind)
           << " " << import.owner;
    if (!import.owner.empty())
      output << ".";
    output << import.name << " : " << hfir::valueTypeName(import.returnType)
           << " (";
    if (import.hasReceiver)
      output << "receiver: handle" << (import.parameterTypes.empty() ? "" : ", ");
    for (std::size_t parameter = 0;
         parameter < import.parameterTypes.size(); ++parameter) {
      if (parameter != 0)
        output << ", ";
      output << hfir::valueTypeName(import.parameterTypes[parameter]);
    }
    output << ") id=" << hex64(import.id);
    if (!import.typeEncoding.empty())
      output << " encoding=" << import.typeEncoding;
    output << '\n';
  }

  output << "functions " << package.functions.size() << '\n';
  for (std::size_t functionIndex = 0;
       functionIndex < package.functions.size(); ++functionIndex) {
    const hfir::Function &function = package.functions[functionIndex];
    output << "  func @" << functionIndex << " " << function.name << " (";
    for (std::size_t parameter = 0;
         parameter < function.parameterTypes.size(); ++parameter) {
      if (parameter != 0)
        output << ", ";
      output << hfir::valueTypeName(function.parameterTypes[parameter]);
    }
    output << ") -> " << hfir::valueTypeName(function.returnType)
           << " registers=" << function.registerTypes.size()
           << " locals=" << function.localTypes.size() << '\n';
    for (const hfir::BasicBlock &block : function.blocks) {
      output << "    block ^" << block.id;
      if (block.id == function.entryBlock)
        output << " entry";
      output << '\n';
      for (const hfir::Instruction &instruction : block.instructions) {
        output << "      ";
        if (instruction.result != hfir::kNoRegister)
          output << "%" << instruction.result << ":"
                 << hfir::valueTypeName(instruction.resultType) << " = ";
        output << hfir::opcodeName(instruction.opcode);
        for (const hfir::Operand &operand : instruction.operands) {
          output << " ";
          switch (operand.kind) {
          case hfir::OperandKind::Register: output << "%"; break;
          case hfir::OperandKind::Constant: output << "#"; break;
          case hfir::OperandKind::Block: output << "^"; break;
          case hfir::OperandKind::Import: output << "!"; break;
          case hfir::OperandKind::Function: output << "@"; break;
          case hfir::OperandKind::Local: output << "$"; break;
          }
          output << operand.index;
          if (operand.type != hfir::ValueType::Void)
            output << ":" << hfir::valueTypeName(operand.type);
        }
        output << '\n';
      }
    }
  }
  output << "debug-locations " << package.debugLocations.size() << '\n';
  output << "signature "
         << (package.signature.bytes.empty()
                 ? "none"
                 : package.signature.algorithm + " key=" +
                       package.signature.keyID + " bytes=" +
                       std::to_string(package.signature.bytes.size()))
         << '\n';
  return output.str();
}

} // namespace irhotfix::container
