#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace llvm;

namespace {
constexpr llvm::StringLiteral HotfixAnnotation = "ir_hotfix_target";

struct NativeTarget {
  std::string symbol;
  bool hasNativeReceiver = false;
};

bool hasHotfixAnnotation(const FunctionDecl &declaration) {
  for (const AnnotateAttr *attribute :
       declaration.specific_attrs<AnnotateAttr>()) {
    if (attribute->getAnnotation() == HotfixAnnotation)
      return true;
  }
  return false;
}

bool isSupportedValueType(ASTContext &context, QualType type,
                          bool allowVoid) {
  type = type.getCanonicalType().getUnqualifiedType();
  if (allowVoid && type->isVoidType())
    return true;
  if (type->isBooleanType() || type->isSpecificBuiltinType(BuiltinType::Float) ||
      type->isSpecificBuiltinType(BuiltinType::Double))
    return true;
  return type->isIntegerType() && context.getTypeSize(type) == 64;
}

class TargetVisitor : public RecursiveASTVisitor<TargetVisitor> {
public:
  explicit TargetVisitor(ASTContext &context)
      : context(context), diagnostics(context.getDiagnostics()),
        mangler(context.createMangleContext()) {}

  bool VisitFunctionDecl(FunctionDecl *declaration) {
    if (!hasHotfixAnnotation(*declaration) ||
        !declaration->doesThisDeclarationHaveABody())
      return true;

    const FunctionDecl *definition = declaration->getDefinition();
    if (definition != declaration)
      return true;

    bool nativeReceiver = false;
    if (const auto *method = dyn_cast<CXXMethodDecl>(declaration)) {
      if (method->isVirtual()) {
        report(method->getLocation(),
               "virtual C++ methods are not supported hotfix targets");
        return true;
      }
      nativeReceiver = method->isInstance();
    }
    if (isa<CXXConstructorDecl>(declaration) ||
        isa<CXXDestructorDecl>(declaration)) {
      report(declaration->getLocation(),
             "C++ constructors and destructors are not supported hotfix targets");
      return true;
    }
    if (declaration->getTemplatedKind() != FunctionDecl::TK_NonTemplate) {
      report(declaration->getLocation(),
             "C++ templates are not supported hotfix targets");
      return true;
    }
    if (declaration->isVariadic()) {
      report(declaration->getLocation(),
             "variadic functions are not supported hotfix targets");
      return true;
    }
    if (!isSupportedValueType(context, declaration->getReturnType(), true)) {
      report(declaration->getLocation(),
             "hotfix target return type must be void, bool, a 64-bit integer, "
             "float, or double");
      return true;
    }
    if (declaration->getNumParams() > 8) {
      report(declaration->getLocation(),
             "hotfix targets support at most eight scalar parameters");
      return true;
    }
    for (const ParmVarDecl *parameter : declaration->parameters()) {
      if (!isSupportedValueType(context, parameter->getType(), false)) {
        report(parameter->getLocation(),
               "hotfix target parameters must be bool, 64-bit integers, "
               "float, or double");
        return true;
      }
    }

    std::string symbol;
    raw_string_ostream stream(symbol);
    if (mangler->shouldMangleDeclName(declaration))
      mangler->mangleName(declaration, stream);
    else
      stream << declaration->getName();
    stream.flush();
    targets.push_back({std::move(symbol), nativeReceiver});
    return true;
  }

  bool write(StringRef outputPath) {
    if (diagnostics.hasErrorOccurred())
      return false;
    llvm::sort(targets, [](const NativeTarget &left, const NativeTarget &right) {
      return left.symbol < right.symbol;
    });
    targets.erase(std::unique(targets.begin(), targets.end(),
                              [](const NativeTarget &left,
                                 const NativeTarget &right) {
                                return left.symbol == right.symbol;
                              }),
                  targets.end());

    std::error_code error;
    raw_fd_ostream output(outputPath, error);
    if (error) {
      report(context.getTranslationUnitDecl()->getLocation(),
             "cannot write native hotfix target metadata: " + error.message());
      return false;
    }
    for (const NativeTarget &target : targets)
      output << target.symbol << '\t'
             << (target.hasNativeReceiver ? "native" : "none") << '\n';
    output.flush();
    return !output.has_error();
  }

private:
  void report(SourceLocation location, const Twine &message) {
    unsigned identifier = diagnostics.getCustomDiagID(
        DiagnosticsEngine::Error, "IR_HOTFIX_TARGET: %0");
    diagnostics.Report(location, identifier) << message.str();
  }

  ASTContext &context;
  DiagnosticsEngine &diagnostics;
  std::unique_ptr<MangleContext> mangler;
  std::vector<NativeTarget> targets;
};

class TargetConsumer : public ASTConsumer {
public:
  TargetConsumer(ASTContext &context, std::string outputPath)
      : visitor(context), outputPath(std::move(outputPath)) {}

  void HandleTranslationUnit(ASTContext &context) override {
    visitor.TraverseDecl(context.getTranslationUnitDecl());
    visitor.write(outputPath);
  }

private:
  TargetVisitor visitor;
  std::string outputPath;
};

class HotfixClangAction : public PluginASTAction {
protected:
  bool ParseArgs(const CompilerInstance &compiler,
                 const std::vector<std::string> &arguments) override {
    if (arguments.size() == 1 && !arguments.front().empty()) {
      outputPath = arguments.front();
      return true;
    }
    DiagnosticsEngine &diagnostics = compiler.getDiagnostics();
    unsigned identifier = diagnostics.getCustomDiagID(
        DiagnosticsEngine::Error,
        "IR hotfix Clang plugin requires one metadata output path");
    diagnostics.Report(identifier);
    return false;
  }

  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &compiler, StringRef) override {
    return std::make_unique<TargetConsumer>(compiler.getASTContext(),
                                            outputPath);
  }

  ActionType getActionType() override { return AddBeforeMainAction; }

private:
  std::string outputPath;
};
} // namespace

static FrontendPluginRegistry::Add<HotfixClangAction>
    registration("ir-hotfix-native-descriptor",
                 "validate and describe annotated C/C++ hotfix targets");
