#if IR_HOTFIX_PATCH_BUILD
@attached(peer, names: prefixed(__ir_hotfix_patch_anchor_))
macro HotfixPatch() = #externalMacro(
    module: "IRHotfixMacrosPlugin",
    type: "HotfixPatchMacro"
)
#endif
