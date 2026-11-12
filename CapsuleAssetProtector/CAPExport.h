#pragma once

// =============================================
// CAPExport
// =============================================
// CapsuleAssetProtectorをDLLとしてビルドする際のエクスポート/インポートマクロです。
// CAP側(DLL本体)をビルドするときはCAP_EXPORTSが定義され、dllexportになります。
// CAPを利用する側(App等)ではCAP_EXPORTSが定義されないので、dllimportになります。
// これにより、assimp等の内部依存を一切見せずにDLLの外へAPIだけを公開できます。
// =============================================

#ifdef CAP_EXPORTS
#define CAP_API __declspec(dllexport)
#else
#define CAP_API __declspec(dllimport)
#endif
