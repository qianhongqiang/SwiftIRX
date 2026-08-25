# Patch 页面示例

`Sources` 中的文件是与 App 示例页面对应的最小 Patch 源码。它们不参与 App 编译，使用已发布版本的 `HotfixTargetManifest.json` 单独生成 `.hfpatch`。

目录内的 `HotfixTargetManifest.json` 是这些固定示例对应的 ABI v3
baseline，可用于重新生成随 App 打包的演示产物。真实发布流程仍应使用
对应发布版本构建阶段导出的 Manifest。

示例产物与页面资源名称对应：

- `HotfixInteger.hfpatch`
- `HotfixBranch.hfpatch`
- `HotfixInstance.hfpatch`
- `HotfixSetupUI.hfpatch`
- `HotfixHostAdapter.hfpatch`
- `HotfixC.hfpatch`
- `HotfixCXX.hfpatch`

Swift examples use `swift-patch-build`. The C and C++ examples use
`clang-patch-build`; C++ is currently limited to non-virtual instance targets
whose `this` pointer is carried as a scoped native handle.

没有同名产物时，页面仍可运行发布版实现，并明确显示缺少的 Patch 文件。
