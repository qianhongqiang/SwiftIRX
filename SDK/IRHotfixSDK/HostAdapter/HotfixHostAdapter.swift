import Foundation

nonisolated enum HotfixHostAdapterError: Error, Equatable, Sendable {
    case registrationFailed(status: HFStatus)
    case invalidHostHandle(status: HFStatus)
    case hostHandleTypeMismatch
}

nonisolated final class HotfixResolvedHostObject<Object: AnyObject> {
    let value: Object
    private let lease: OpaquePointer

    init(value: Object, lease: OpaquePointer) {
        self.value = value
        self.lease = lease
    }

    deinit {
        hf_host_handle_lease_release(lease)
    }
}

nonisolated enum HotfixHostHandle {
    static func resolveObject<Object: AnyObject>(
        _ handle: HFHandle,
        as type: Object.Type
    ) throws -> HotfixResolvedHostObject<Object> {
        var lease: OpaquePointer?
        var pointer: UnsafeMutableRawPointer?
        let status = hf_host_handle_resolve(handle, &lease, &pointer)
        guard status == HFStatus(HFStatusApplied),
              let lease,
              let pointer else {
            throw HotfixHostAdapterError.invalidHostHandle(status: status)
        }
        let candidate = Unmanaged<AnyObject>
            .fromOpaque(UnsafeRawPointer(pointer))
            .takeUnretainedValue()
        guard let object = candidate as? Object else {
            hf_host_handle_lease_release(lease)
            throw HotfixHostAdapterError.hostHandleTypeMismatch
        }
        return HotfixResolvedHostObject(value: object, lease: lease)
    }
}

nonisolated struct HotfixSwiftHostCall: Sendable {
    let symbol: String
    let owner: String
    let callKind: HFHostCallKind
    let returnKind: HFValueKind
    let argumentKinds: [HFValueKind]
    let hasReceiver: Bool
    let mainThreadOnly: Bool
    let objcCompatibleHandles: Bool
    let noSideEffects: Bool

    init(
        symbol: String,
        owner: String = "",
        callKind: HFHostCallKind = HFHostCallKind(HFHostCallKindFunction),
        returnKind: HFValueKind,
        argumentKinds: [HFValueKind],
        hasReceiver: Bool = false,
        mainThreadOnly: Bool = false,
        objcCompatibleHandles: Bool = false,
        noSideEffects: Bool = false
    ) {
        self.symbol = symbol
        self.owner = owner
        self.callKind = callKind
        self.returnKind = returnKind
        self.argumentKinds = argumentKinds
        self.hasReceiver = hasReceiver
        self.mainThreadOnly = mainThreadOnly
        self.objcCompatibleHandles = objcCompatibleHandles
        self.noSideEffects = noSideEffects
    }
}

private nonisolated final class HotfixSwiftHostAdapterBox: @unchecked Sendable {
    let handler: @Sendable (HFHandle, [HFValue]) throws -> HFValue

    init(handler: @escaping @Sendable (HFHandle, [HFValue]) throws -> HFValue) {
        self.handler = handler
    }
}

@_cdecl("irhf_swift_host_adapter_entry")
private nonisolated func irhfSwiftHostAdapterEntry(
    _ framePointer: UnsafeMutablePointer<HFHostCallFrame>?
) -> HFStatus {
    guard let framePointer else { return HFStatus(HFStatusInvalidFrame) }
    let frame = framePointer.pointee
    guard frame.abiVersion == UInt32(HF_HOST_ADAPTER_ABI_VERSION),
          frame.structSize == UInt32(MemoryLayout<HFHostCallFrame>.size),
          frame.reserved0 == 0,
          frame.reserved1 == 0,
          let context = frame.context,
          frame.argumentCount == 0 || frame.arguments != nil else {
        return HFStatus(HFStatusInvalidFrame)
    }
    let box = Unmanaged<HotfixSwiftHostAdapterBox>
        .fromOpaque(context)
        .takeUnretainedValue()
    let arguments = frame.argumentCount == 0
        ? []
        : Array(UnsafeBufferPointer(
            start: frame.arguments,
            count: Int(frame.argumentCount)
        ))
    do {
        framePointer.pointee.result = try box.handler(frame.receiver, arguments)
        framePointer.pointee.status = HFStatus(HFStatusApplied)
        return HFStatus(HFStatusApplied)
    } catch {
        framePointer.pointee.status = HFStatus(HFStatusExecutionFailed)
        return HFStatus(HFStatusExecutionFailed)
    }
}

@_cdecl("irhf_swift_host_adapter_release")
private nonisolated func irhfSwiftHostAdapterRelease(_ context: UnsafeMutableRawPointer?) {
    guard let context else { return }
    Unmanaged<HotfixSwiftHostAdapterBox>.fromOpaque(context).release()
}

nonisolated final class HotfixHostAdapterRegistration: @unchecked Sendable {
    let importID: UInt64
    let signatureID: UInt64
    private let lock = NSLock()
    private var registration: HFHostAdapterRegistration

    fileprivate init(registration: HFHostAdapterRegistration) {
        self.registration = registration
        self.importID = registration.importID
        self.signatureID = registration.signatureID
    }

    func unregister() {
        lock.lock()
        let removed = registration
        registration = HFInvalidHostAdapterRegistration()
        lock.unlock()
        guard removed.token != 0 else { return }
        _ = hf_host_adapter_unregister(removed)
    }

    deinit {
        unregister()
    }
}

nonisolated enum HotfixSwiftHostAdapter {
    static func register(
        _ call: HotfixSwiftHostCall,
        handler: @escaping @Sendable (HFHandle, [HFValue]) throws -> HFValue
    ) throws -> HotfixHostAdapterRegistration {
        let box = HotfixSwiftHostAdapterBox(handler: handler)
        let retainedContext = Unmanaged.passRetained(box).toOpaque()

        var status = HFStatus(HFStatusInvalidArguments)
        var registration = HFInvalidHostAdapterRegistration()
        call.symbol.withCString { symbol in
            call.owner.withCString { owner in
                call.argumentKinds.withUnsafeBufferPointer { kinds in
                    var descriptor = HFHostCallDescriptor()
                    descriptor.abiVersion = UInt32(HF_HOST_ADAPTER_ABI_VERSION)
                    descriptor.structSize = UInt32(MemoryLayout<HFHostCallDescriptor>.size)
                    descriptor.importID = hf_host_call_id(symbol)
                    descriptor.language = HFHostLanguage(HFHostLanguageSwift)
                    descriptor.callKind = call.callKind
                    descriptor.returnKind = call.returnKind
                    descriptor.argumentCount = UInt32(kinds.count)
                    var flags = UInt32(HFHostCallFlagNone)
                    if call.hasReceiver { flags |= UInt32(HFHostCallFlagHasReceiver) }
                    if call.mainThreadOnly { flags |= UInt32(HFHostCallFlagMainThreadOnly) }
                    if call.objcCompatibleHandles {
                        flags |= UInt32(HFHostCallFlagObjCCompatibleHandles)
                    }
                    if call.noSideEffects {
                        flags |= UInt32(HFHostCallFlagNoSideEffects)
                    }
                    descriptor.flags = flags
                    descriptor.owner = owner
                    descriptor.name = symbol
                    descriptor.typeEncoding = nil
                    descriptor.argumentKinds = kinds.baseAddress
                    descriptor.signatureID = hf_host_call_signature_id(&descriptor)
                    status = hf_host_adapter_register(
                        &descriptor,
                        irhfSwiftHostAdapterEntry,
                        retainedContext,
                        irhfSwiftHostAdapterRelease,
                        &registration
                    )
                }
            }
        }
        guard status == HFStatus(HFStatusApplied) else {
            Unmanaged<HotfixSwiftHostAdapterBox>.fromOpaque(retainedContext).release()
            throw HotfixHostAdapterError.registrationFailed(status: status)
        }
        return HotfixHostAdapterRegistration(registration: registration)
    }
}
