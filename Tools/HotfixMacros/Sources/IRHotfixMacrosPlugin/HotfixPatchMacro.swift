import SwiftCompilerPlugin
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

private struct HotfixPatchMacroError: Error, CustomStringConvertible {
    let description: String
}

public struct HotfixPatchMacro: PeerMacro {
    public static func expansion(
        of node: AttributeSyntax,
        providingPeersOf declaration: some DeclSyntaxProtocol,
        in context: some MacroExpansionContext
    ) throws -> [DeclSyntax] {
        guard let function = declaration.as(FunctionDeclSyntax.self),
              function.body != nil else {
            throw HotfixPatchMacroError(
                description: "@HotfixPatch can only be attached to a function with a body"
            )
        }
        guard function.genericParameterClause == nil,
              function.genericWhereClause == nil else {
            throw HotfixPatchMacroError(
                description: "@HotfixPatch does not support generic functions"
            )
        }
        guard function.signature.effectSpecifiers == nil else {
            throw HotfixPatchMacroError(
                description: "@HotfixPatch does not support async or throwing functions"
            )
        }
        if function.modifiers.contains(where: {
            ["class", "static", "mutating", "nonmutating"].contains($0.name.text)
        }) {
            throw HotfixPatchMacroError(
                description: "@HotfixPatch currently supports top-level and class instance functions"
            )
        }

        let functionName = function.name.text
        let anchorName = context.makeUniqueName(
            "__ir_hotfix_patch_anchor_\(functionName)"
        ).text
        let arguments = try function.signature.parameterClause.parameters.map {
            parameter -> String in
            let localName = parameter.secondName ?? parameter.firstName
            guard localName.text != "_" else {
                throw HotfixPatchMacroError(
                    description: "@HotfixPatch parameters must have local names"
                )
            }
            if parameter.firstName.text == "_" {
                return localName.text
            }
            return "\(parameter.firstName.text): \(localName.text)"
        }.joined(separator: ", ")

        let parameters = function.signature.parameterClause.trimmedDescription
        let returnClause = function.signature.returnClause.map {
            " \($0.trimmedDescription)"
        } ?? ""
        let returnPrefix = function.signature.returnClause == nil ? "" : "return "
        return [
            DeclSyntax(
                stringLiteral: """
                @inline(never)
                private func \(anchorName)\(parameters)\(returnClause) {
                    \(returnPrefix)\(functionName)(\(arguments))
                }
                """
            )
        ]
    }
}

@main
struct IRHotfixMacrosPlugin: CompilerPlugin {
    let providingMacros: [Macro.Type] = [
        HotfixPatchMacro.self
    ]
}
