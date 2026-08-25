#ifndef IRHotfix_HFIRLowering_h
#define IRHotfix_HFIRLowering_h

#include "HFIR.h"

#include <cstdint>
#include <string>

namespace llvm {
class Function;
class Module;
}

namespace irhotfix::lowering {

bool lowerFunction(llvm::Module &module, llvm::Function &function,
                   std::uint64_t targetID, std::uint64_t signatureID,
                   const hfir::TargetABISchema &targetABI,
                   hfir::Package &package, std::string &error);

} // namespace irhotfix::lowering

#endif /* IRHotfix_HFIRLowering_h */
