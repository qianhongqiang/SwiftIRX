#if __has_include("HotfixInstrumentationStamp.h")
#include "HotfixInstrumentationStamp.h"
#endif

#include "../ABI/IRHotfixABI.h"
#include "../Bridge/IRHotfixObjCBridge.h"
#include "../HostHandle/HFHostHandleTable.h"
#include "../HostAdapter/HFHostAdapter.h"
#include "../HostAdapter/HFGeneratedHostAdapters.h"
#include "../VM/HFIRRuntime.h"
#include "../Runtime/HFNativePatchRuntime.h"
#include "../../../IR/NativeExamples/HotfixNativeExamples.h"
