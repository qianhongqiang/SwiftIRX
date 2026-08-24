# Patch 页面示例

`Sources` 中的文件是与 App 示例页面对应的最小 Patch 源码。它们不参与 App 编译，使用已发布版本的 `HotfixTargetManifest.json` 单独生成 `.hfpatch`。

示例产物与页面资源名称对应：

- `HotfixInteger.hfpatch`
- `HotfixBranch.hfpatch`
- `HotfixInstance.hfpatch`
- `HotfixSetupUI.hfpatch`
- `HotfixHostAdapter.hfpatch`

没有同名产物时，页面仍可运行发布版实现，并明确显示缺少的 Patch 文件。
