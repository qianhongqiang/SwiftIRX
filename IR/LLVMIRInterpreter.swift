import Foundation
#if canImport(UIKit)
import UIKit
#endif

nonisolated enum LLVMIRInterpreterError: Error, Equatable, Sendable {
    case parse(String)
    case runtime(String)
}

nonisolated enum LLVMInvocationValue: Equatable, Sendable {
    case int(Int)
    case bool(Bool)
    case pointer(Int)
}

nonisolated enum LLVMInvocationResult: Equatable, Sendable {
    case int(Int)
    case bool(Bool)
    case pointer(Int)
    case void
}

private nonisolated let llvmMaximumAggregateElementCount = 1_024

nonisolated private func llvmStableSymbolAddress(_ symbol: String) -> Int {
    var hash = 5381
    for scalar in symbol.unicodeScalars {
        hash = ((hash << 5) &+ hash) &+ Int(scalar.value)
    }
    return 2_000_000 + abs(hash % 1_000_000)
}

#if canImport(UIKit)
nonisolated private func withUIKitOnMainActor<T: Sendable>(
    _ body: @MainActor () throws -> T
) throws -> T {
    guard Thread.isMainThread else {
        throw LLVMIRInterpreterError.runtime("UIKit bridge requires the main thread.")
    }
    return try MainActor.assumeIsolated(body)
}
#endif

// The host is borrowed weakly and read only during a synchronous interpreter invocation.
nonisolated final class LLVMHostContext: @unchecked Sendable {
    weak var rootObject: AnyObject?

    init(rootObject: AnyObject?) {
        self.rootObject = rootObject
    }

#if canImport(UIKit)
    init(rootViewController: UIViewController?) {
        self.rootObject = rootViewController
    }
#else
    init(rootViewController: AnyObject?) {
        self.rootObject = rootViewController
    }
#endif
}

nonisolated final class LLVMIRInterpreter: Sendable {
    func run(
        ir: String,
        function name: String,
        arguments: [LLVMInvocationValue],
        host: LLVMHostContext? = nil
    ) throws -> LLVMInvocationResult {
        let module = try LLVMIRParser().parseModule(ir: ir)
        guard let function = module.functions[name] else {
            throw LLVMIRInterpreterError.parse("Missing @\(name) function.")
        }

        let state = LLVMRuntimeState(hostGlobals: module.hostGlobals)
        var runtimeArguments = arguments.map { value in
            switch value {
            case let .int(value):
                return LLVMValue.int(value)
            case let .bool(value):
                return LLVMValue.bool(value)
            case let .pointer(value):
                return LLVMValue.pointer(value)
            }
        }
        if let rootObject = host?.rootObject,
           function.parameters.first?.type == .ptr,
           runtimeArguments.indices.contains(0),
           case .pointer(0) = runtimeArguments[0] {
            runtimeArguments[0] = .hostHandle(
                state.registerObject(rootObject, symbol: "$host.root")
            )
        }
        let budget = LLVMExecutionBudget()
        try budget.enterFunction()
        defer { budget.leaveFunction() }
        let result = try execute(
            function: function,
            module: module,
            state: state,
            budget: budget,
            arguments: runtimeArguments,
            host: host
        )

        switch (function.returnType, result) {
        case (.void, nil):
            return .void
        case (.void, .some):
            throw LLVMIRInterpreterError.runtime("@\(name) returned a value, expected void.")
        case (.type(.i8), .some(.int(let value))),
             (.type(.i32), .some(.int(let value))),
             (.type(.i64), .some(.int(let value))):
            return .int(value)
        case (.type(.i1), .some(.bool(let value))):
            return .bool(value)
        case (.type(.ptr), .some(.pointer(let value))):
            return .pointer(value)
        case (.type(.ptr), .some(.hostHandle(let handle))):
            return .pointer(handle.address)
        case (.type, nil):
            throw LLVMIRInterpreterError.runtime("@\(name) returned void, expected a value.")
        case (.type, .some):
            throw LLVMIRInterpreterError.runtime("Return type mismatch in @\(name).")
        }
    }

    func runMain(ir: String, host: LLVMHostContext? = nil) throws -> Int {
        let result = try run(ir: ir, function: "main", arguments: [], host: host)
        switch result {
        case let .int(value):
            return value
        case let .bool(value):
            return value ? 1 : 0
        case .pointer:
            throw LLVMIRInterpreterError.runtime("@main returned pointer, expected i32.")
        case .void:
            throw LLVMIRInterpreterError.runtime("@main returned void, expected i32.")
        }
    }

    private func execute(
        function: LLVMFunction,
        module: LLVMModule,
        state: LLVMRuntimeState,
        budget: LLVMExecutionBudget,
        arguments: [LLVMValue],
        host: LLVMHostContext?
    ) throws -> LLVMValue? {
        guard arguments.count == function.parameters.count else {
            throw LLVMIRInterpreterError.runtime(
                "Function @\(function.name) expected \(function.parameters.count) arguments, got \(arguments.count)."
            )
        }

        guard let entry = function.blocks.first?.label else {
            throw LLVMIRInterpreterError.runtime("Function \(function.name) has no basic blocks.")
        }

        var env: [String: LLVMValue] = [:]
        for (parameter, value) in zip(function.parameters, arguments) {
            guard value.matches(type: parameter.type) else {
                throw LLVMIRInterpreterError.runtime(
                    "Function @\(function.name) argument type mismatch for %\(parameter.name)."
                )
            }
            env[parameter.name] = value
        }
        var blockMap: [String: LLVMBasicBlock] = [:]
        for block in function.blocks {
            guard blockMap[block.label] == nil else {
                throw LLVMIRInterpreterError.runtime(
                    "Duplicate basic block label %\(block.label) in function @\(function.name)."
                )
            }
            blockMap[block.label] = block
        }
        var currentLabel = entry
        var predecessor: String?

        while true {
            try budget.consumeStep()

            guard let block = blockMap[currentLabel] else {
                throw LLVMIRInterpreterError.runtime("Unknown block label %\(currentLabel).")
            }

            // PHI values are selected from predecessor and read from snapshot.
            let snapshot = env
            var pendingPhiWrites: [(String, LLVMValue)] = []
            for instruction in block.instructions {
                guard case let .phi(result, incomings) = instruction else { break }
                guard let pred = predecessor else {
                    throw LLVMIRInterpreterError.runtime("PHI %\(result) requires predecessor.")
                }
                guard let incoming = incomings.first(where: { $0.label == pred }) else {
                    throw LLVMIRInterpreterError.runtime("PHI %\(result) has no incoming from %\(pred).")
                }
                let value = try resolve(operand: incoming.value, env: snapshot)
                pendingPhiWrites.append((result, value))
            }
            for (name, value) in pendingPhiWrites {
                env[name] = value
            }

            var jumped = false
            for instruction in block.instructions {
                switch instruction {
                case .phi:
                    continue
                case let .add(result, lhs, rhs):
                    let value = try intBinOp(lhs: lhs, rhs: rhs, env: env, &+)
                    env[result] = .int(value)
                case let .sub(result, lhs, rhs):
                    let value = try intBinOp(lhs: lhs, rhs: rhs, env: env, &-)
                    env[result] = .int(value)
                case let .mul(result, lhs, rhs):
                    let value = try intBinOp(lhs: lhs, rhs: rhs, env: env, &*)
                    env[result] = .int(value)
                case let .sdiv(result, lhs, rhs):
                    let left = try resolveInt(lhs, env: env)
                    let right = try resolveInt(rhs, env: env)
                    guard right != 0 else {
                        throw LLVMIRInterpreterError.runtime("Division by zero in sdiv.")
                    }
                    guard left != Int.min || right != -1 else {
                        throw LLVMIRInterpreterError.runtime("Signed division overflow in sdiv.")
                    }
                    env[result] = .int(left / right)
                case let .and(result, type, lhs, rhs):
                    env[result] = try evalBitwise(type: type, lhs: lhs, rhs: rhs, env: env, op: .and)
                case let .or(result, type, lhs, rhs):
                    env[result] = try evalBitwise(type: type, lhs: lhs, rhs: rhs, env: env, op: .or)
                case let .xor(result, type, lhs, rhs):
                    env[result] = try evalBitwise(type: type, lhs: lhs, rhs: rhs, env: env, op: .xor)
                case let .shl(result, type, lhs, rhs):
                    env[result] = try evalShift(type: type, lhs: lhs, rhs: rhs, env: env, op: .shl)
                case let .ashr(result, type, lhs, rhs):
                    env[result] = try evalShift(type: type, lhs: lhs, rhs: rhs, env: env, op: .ashr)
                case let .zext(result, from, operand, to):
                    env[result] = try evalCast(kind: "zext", from: from, operand: operand, to: to, env: env)
                case let .sext(result, from, operand, to):
                    env[result] = try evalCast(kind: "sext", from: from, operand: operand, to: to, env: env)
                case let .trunc(result, from, operand, to):
                    env[result] = try evalCast(kind: "trunc", from: from, operand: operand, to: to, env: env)
                case let .ptrtoint(result, value, to):
                    guard to == .i64 || to == .i32 else {
                        throw LLVMIRInterpreterError.runtime("ptrtoint supports only i32/i64 targets.")
                    }
                    let pointer = try resolvePointer(value, env: env)
                    env[result] = .int(pointer)
                case let .inttoptr(result, value, to):
                    guard to == .ptr else {
                        throw LLVMIRInterpreterError.runtime("inttoptr target must be ptr.")
                    }
                    let integer = try resolveInt(value, env: env)
                    env[result] = state.pointerValue(for: integer)
                case let .extractvalue(result, aggregate, index):
                    let value = try resolve(operand: aggregate, env: env)
                    switch value {
                    case let .aggregate(elements):
                        guard index >= 0, index < elements.count else {
                            throw LLVMIRInterpreterError.runtime("extractvalue index out of range.")
                        }
                        env[result] = elements[index]
                    default:
                        guard index == 0 else {
                            throw LLVMIRInterpreterError.runtime("extractvalue on scalar supports only index 0.")
                        }
                        env[result] = value
                    }
                case let .insertvalue(result, aggregate, element, index):
                    guard index >= 0, index < llvmMaximumAggregateElementCount else {
                        throw LLVMIRInterpreterError.runtime("insertvalue index out of range.")
                    }
                    let elementValue = try resolve(operand: element, env: env)
                    if case .undef = aggregate {
                        if index == 0 {
                            env[result] = elementValue
                        } else {
                            var aggregateValues = Array(repeating: LLVMValue.int(0), count: index + 1)
                            aggregateValues[index] = elementValue
                            env[result] = .aggregate(aggregateValues)
                        }
                        break
                    }

                    let baseValue = try resolve(operand: aggregate, env: env)
                    switch baseValue {
                    case var .aggregate(values):
                        if index >= values.count {
                            values.append(contentsOf: Array(repeating: LLVMValue.int(0), count: index - values.count + 1))
                        }
                        values[index] = elementValue
                        env[result] = .aggregate(values)
                    default:
                        // Pragmatic scalar fallback: many Swift aggregate paths only care about field 0.
                        if index == 0 {
                            env[result] = elementValue
                        } else {
                            env[result] = baseValue
                        }
                    }
                case let .getelementptr(result, base, indices):
                    let baseAddress = try resolvePointer(base, env: env)
                    let resolvedIndices = try indices.map { try resolveInt($0, env: env) }
                    let derivedAddress = state.derivedPointer(base: baseAddress, indices: resolvedIndices)
                    env[result] = .pointer(derivedAddress)
                case let .icmp(result, type, predicate, lhs, rhs):
                    let left = try resolveComparable(type: type, operand: lhs, env: env)
                    let right = try resolveComparable(type: type, operand: rhs, env: env)
                    env[result] = .bool(evaluate(predicate: predicate, lhs: left, rhs: right))
                case let .alloca(result, allocatedType):
                    let address = state.nextAddress
                    state.nextAddress += 1
                    state.memory[address] = defaultValue(for: allocatedType)
                    env[result] = .pointer(address)
                case let .store(value, pointer):
                    let address = try resolvePointer(pointer, env: env)
                    let stored = try resolve(operand: value, env: env)
                    state.memory[address] = stored
                case let .load(result, _, pointer):
                    let address = try resolvePointer(pointer, env: env)
                    if let loaded = state.memory[address] {
                        env[result] = loaded
                    } else if let loaded = try state.loadHostGlobal(at: address) {
                        env[result] = loaded
                    } else {
                        throw LLVMIRInterpreterError.runtime("Load from uninitialized address \(address).")
                    }
                case let .call(result, functionName, arguments, returnType):
                    let resolvedArguments = try arguments.map { try resolve(operand: $0, env: env) }
                    if let callee = module.functions[functionName] {
                        guard returnType.isCompatible(with: callee.returnType) else {
                            throw LLVMIRInterpreterError.runtime(
                                "Call return type mismatch for function @\(functionName)."
                            )
                        }
                        try budget.enterFunction()
                        let callResult: LLVMValue?
                        do {
                            defer { budget.leaveFunction() }
                            callResult = try execute(
                                function: callee,
                                module: module,
                                state: state,
                                budget: budget,
                                arguments: resolvedArguments,
                                host: host
                            )
                        }
                        try bindCallResult(
                            resultName: result,
                            functionName: functionName,
                            returnType: returnType,
                            callResult: callResult,
                            env: &env
                        )
                    } else if let externalResult = try executeExternalCall(
                        functionName: functionName,
                        arguments: resolvedArguments,
                        declaredReturnType: returnType,
                        state: state,
                        host: host
                    ) {
                        try bindCallResult(
                            resultName: result,
                            functionName: functionName,
                            returnType: returnType,
                            callResult: externalResult,
                            env: &env
                        )
                    } else {
                        throw LLVMIRInterpreterError.runtime("Call to unknown function @\(functionName).")
                    }
                case let .inlineAsm(result, returnType):
                    try bindCallResult(
                        resultName: result,
                        functionName: "asm",
                        returnType: returnType,
                        callResult: defaultInlineAsmReturnValue(for: returnType),
                        env: &env
                    )
                case let .br(label):
                    predecessor = currentLabel
                    currentLabel = label
                    jumped = true
                case let .brCond(cond, trueLabel, falseLabel):
                    let condition = try resolveBool(cond, env: env)
                    predecessor = currentLabel
                    currentLabel = condition ? trueLabel : falseLabel
                    jumped = true
                case let .ret(operand):
                    let value = try resolve(operand: operand, env: env)
                    guard function.returnType.matches(value: value) else {
                        throw LLVMIRInterpreterError.runtime("Return type mismatch in @\(function.name).")
                    }
                    return value
                case .retVoid:
                    guard case .void = function.returnType else {
                        throw LLVMIRInterpreterError.runtime("Void return in non-void function @\(function.name).")
                    }
                    return nil
                case .unreachable:
                    throw LLVMIRInterpreterError.runtime("Reached unreachable instruction.")
                }

                if jumped { break }
            }

            if !jumped {
                throw LLVMIRInterpreterError.runtime("Block %\(currentLabel) has no terminator.")
            }
        }
    }

    private func intBinOp(
        lhs: LLVMOperand,
        rhs: LLVMOperand,
        env: [String: LLVMValue],
        _ op: (Int, Int) -> Int
    ) throws -> Int {
        let left = try resolveInt(lhs, env: env)
        let right = try resolveInt(rhs, env: env)
        return op(left, right)
    }

    private func resolve(operand: LLVMOperand, env: [String: LLVMValue]) throws -> LLVMValue {
        switch operand {
        case let .int(v):
            return .int(v)
        case let .bool(v):
            return .bool(v)
        case let .pointer(address):
            return .pointer(address)
        case .undef:
            throw LLVMIRInterpreterError.runtime("Cannot resolve undef without instruction-specific handling.")
        case let .local(name):
            guard let value = env[name] else {
                throw LLVMIRInterpreterError.runtime("Use of undefined value %\(name).")
            }
            return value
        }
    }

    private func resolveInt(_ operand: LLVMOperand, env: [String: LLVMValue]) throws -> Int {
        let value = try resolve(operand: operand, env: env)
        guard case let .int(v) = value else {
            throw LLVMIRInterpreterError.runtime("Expected i32 integer value.")
        }
        return v
    }

    private func resolveBool(_ operand: LLVMOperand, env: [String: LLVMValue]) throws -> Bool {
        let value = try resolve(operand: operand, env: env)
        guard case let .bool(v) = value else {
            throw LLVMIRInterpreterError.runtime("Expected i1 boolean value.")
        }
        return v
    }

    private func resolvePointer(_ operand: LLVMOperand, env: [String: LLVMValue]) throws -> Int {
        let value = try resolve(operand: operand, env: env)
        switch value {
        case let .pointer(address):
            return address
        case let .hostHandle(handle):
            return handle.address
        default:
            throw LLVMIRInterpreterError.runtime("Expected pointer value.")
        }
    }

    private func resolveComparable(type: LLVMType, operand: LLVMOperand, env: [String: LLVMValue]) throws -> Int {
        switch type {
        case .i8, .i32, .i64:
            return try resolveInt(operand, env: env)
        case .ptr:
            return try resolvePointer(operand, env: env)
        case .i1:
            let value = try resolveBool(operand, env: env)
            return value ? 1 : 0
        }
    }

    private func defaultValue(for type: LLVMType) -> LLVMValue {
        switch type {
        case .i8:
            return .int(0)
        case .i32:
            return .int(0)
        case .i64:
            return .int(0)
        case .i1:
            return .bool(false)
        case .ptr:
            return .pointer(0)
        }
    }

    private func evaluate(predicate: LLVMICmpPredicate, lhs: Int, rhs: Int) -> Bool {
        switch predicate {
        case .eq: return lhs == rhs
        case .ne: return lhs != rhs
        case .slt: return lhs < rhs
        case .sle: return lhs <= rhs
        case .sgt: return lhs > rhs
        case .sge: return lhs >= rhs
        }
    }

    private enum LLVMBinaryBitwiseOp {
        case and
        case or
        case xor
    }

    private enum LLVMShiftOp {
        case shl
        case ashr
    }

    private func evalBitwise(
        type: LLVMType,
        lhs: LLVMOperand,
        rhs: LLVMOperand,
        env: [String: LLVMValue],
        op: LLVMBinaryBitwiseOp
    ) throws -> LLVMValue {
        switch type {
        case .i8:
            let left = try resolveInt(lhs, env: env)
            let right = try resolveInt(rhs, env: env)
            switch op {
            case .and: return .int(left & right)
            case .or: return .int(left | right)
            case .xor: return .int(left ^ right)
            }
        case .i32:
            let left = try resolveInt(lhs, env: env)
            let right = try resolveInt(rhs, env: env)
            switch op {
            case .and: return .int(left & right)
            case .or: return .int(left | right)
            case .xor: return .int(left ^ right)
            }
        case .i64:
            let left = try resolveInt(lhs, env: env)
            let right = try resolveInt(rhs, env: env)
            switch op {
            case .and: return .int(left & right)
            case .or: return .int(left | right)
            case .xor: return .int(left ^ right)
            }
        case .i1:
            let left = try resolveBool(lhs, env: env)
            let right = try resolveBool(rhs, env: env)
            switch op {
            case .and: return .bool(left && right)
            case .or: return .bool(left || right)
            case .xor: return .bool(left != right)
            }
        case .ptr:
            throw LLVMIRInterpreterError.runtime("Bitwise operations on ptr are unsupported.")
        }
    }

    private func evalShift(
        type: LLVMType,
        lhs: LLVMOperand,
        rhs: LLVMOperand,
        env: [String: LLVMValue],
        op: LLVMShiftOp
    ) throws -> LLVMValue {
        guard type == .i32 else {
            throw LLVMIRInterpreterError.runtime("Shift operations currently support only i32.")
        }
        let left = try resolveInt(lhs, env: env)
        let right = try resolveInt(rhs, env: env)
        guard right >= 0 else {
            throw LLVMIRInterpreterError.runtime("Shift amount must be non-negative.")
        }
        let shift = right & 31
        switch op {
        case .shl:
            return .int(left << shift)
        case .ashr:
            return .int(left >> shift)
        }
    }

    private func evalCast(
        kind: String,
        from: LLVMType,
        operand: LLVMOperand,
        to: LLVMType,
        env: [String: LLVMValue]
    ) throws -> LLVMValue {
        switch (kind, from, to) {
        case ("zext", .i1, .i32):
            let value = try resolveBool(operand, env: env)
            return .int(value ? 1 : 0)
        case ("sext", .i1, .i32):
            let value = try resolveBool(operand, env: env)
            return .int(value ? -1 : 0)
        case ("trunc", .i32, .i1):
            let value = try resolveInt(operand, env: env)
            return .bool((value & 1) != 0)
        default:
            throw LLVMIRInterpreterError.runtime("Unsupported cast \(kind) from \(from) to \(to).")
        }
    }

    private func bindCallResult(
        resultName: String?,
        functionName: String,
        returnType: LLVMFunctionReturnType,
        callResult: LLVMValue?,
        env: inout [String: LLVMValue]
    ) throws {
        switch returnType {
        case .void:
            if resultName != nil {
                throw LLVMIRInterpreterError.runtime(
                    "Call to void function @\(functionName) cannot assign to SSA value."
                )
            }
        case let .type(expectedType):
            guard let unwrapped = callResult else {
                throw LLVMIRInterpreterError.runtime(
                    "Function @\(functionName) returned void, expected value."
                )
            }
            guard unwrapped.matches(type: expectedType) else {
                throw LLVMIRInterpreterError.runtime(
                    "Call result type mismatch for function @\(functionName)."
                )
            }
            guard let name = resultName else {
                throw LLVMIRInterpreterError.runtime(
                    "Call to non-void function @\(functionName) must assign result."
                )
            }
            env[name] = unwrapped
        }
    }

    private func executeExternalCall(
        functionName: String,
        arguments: [LLVMValue],
        declaredReturnType: LLVMFunctionReturnType,
        state: LLVMRuntimeState,
        host: LLVMHostContext?
    ) throws -> LLVMValue?? {
        if functionName.hasPrefix("llvm.memset.") ||
            functionName.hasPrefix("llvm.lifetime.start") ||
            functionName.hasPrefix("llvm.lifetime.end") ||
            functionName == "llvm.objc.release" {
            return .some(nil)
        }

        if functionName == "llvm.objc.retain" ||
            functionName == "llvm.objc.retainAutoreleasedReturnValue" ||
            functionName == "objc_opt_self" {
            guard let first = arguments.first else {
                throw LLVMIRInterpreterError.runtime("External \(functionName) expects one argument.")
            }
            return .some(first)
        }

        if functionName.contains("$sSo6UIViewCMa") {
            return .some(try state.classReference(named: "UIView"))
        }

        if functionName.contains("$sSo6UIViewC5frameABSo6CGRectV_tcfC") {
            return .some(try makeUIViewFromFrameCall(arguments: arguments, state: state))
        }

        if functionName == "objc_msgSendSuper2" {
            return .some(nil)
        }

        if functionName == "objc_msgSend" {
            return .some(try executeObjcMessageSend(arguments: arguments, declaredReturnType: declaredReturnType, state: state, host: host))
        }

        return nil
    }

    private func defaultInlineAsmReturnValue(for returnType: LLVMFunctionReturnType) -> LLVMValue? {
        switch returnType {
        case .void:
            return nil
        case .type(.ptr):
            return .pointer(0)
        case .type(.i1):
            return .bool(false)
        case .type(.i8), .type(.i32), .type(.i64):
            return .int(0)
        }
    }

    private func makeUIViewFromFrameCall(arguments: [LLVMValue], state: LLVMRuntimeState) throws -> LLVMValue {
#if canImport(UIKit)
        guard arguments.count >= 4 else {
            throw LLVMIRInterpreterError.runtime("UIView(frame:) bridge expects at least 4 arguments.")
        }
        let x = CGFloat(try scalarFromValue(arguments[0]))
        let y = CGFloat(try scalarFromValue(arguments[1]))
        let w = CGFloat(try scalarFromValue(arguments[2]))
        let h = CGFloat(try scalarFromValue(arguments[3]))
        return try withUIKitOnMainActor {
            let view = UIView(frame: CGRect(x: x, y: y, width: w, height: h))
            return .hostHandle(state.registerObject(view))
        }
#else
        return .pointer(state.allocateExternalPointer())
#endif
    }

    private func executeObjcMessageSend(
        arguments: [LLVMValue],
        declaredReturnType: LLVMFunctionReturnType,
        state: LLVMRuntimeState,
        host: LLVMHostContext?
    ) throws -> LLVMValue? {
#if canImport(UIKit)
        return try withUIKitOnMainActor {
            guard arguments.count >= 2 else {
                throw LLVMIRInterpreterError.runtime("objc_msgSend expects a receiver and selector.")
            }

            let receiver = arguments[0]
            let selectorValue = arguments[1]
            guard let selector = state.selectorName(for: selectorValue) else {
                throw LLVMIRInterpreterError.runtime("objc_msgSend selector is not a structured Host Handle.")
            }

            if case .pointer(0) = receiver {
                return nilObjcMessageResult(for: declaredReturnType)
            }

            guard let receiverObject = state.object(for: receiver) else {
                throw LLVMIRInterpreterError.runtime("objc_msgSend receiver is not a live object.")
            }

            let bridgeArguments = try arguments.dropFirst(2).map {
                try state.objcBridgeValue(for: $0)
            }
            let invocation = selector.withCString { selectorName in
                bridgeArguments.withUnsafeBufferPointer { buffer in
                    IRHFObjCInvoke(
                        Unmanaged.passUnretained(receiverObject).toOpaque(),
                        selectorName,
                        buffer.baseAddress,
                        buffer.count
                    )
                }
            }
            guard invocation.status.rawValue == 0 else {
                let message = String(cString: IRHFObjCInvocationStatusDescription(invocation.status))
                throw LLVMIRInterpreterError.runtime(
                    "Objective-C call \(selector) failed: \(message)."
                )
            }
            let result = try state.value(from: invocation.value)
            switch (declaredReturnType, result) {
            case (.void, nil):
                return nil
            case (.type(let expected), .some(let value)) where value.matches(type: expected):
                return value
            default:
                throw LLVMIRInterpreterError.runtime(
                    "Objective-C call \(selector) returned a value incompatible with its LLVM declaration."
                )
            }
        }
#else
        throw LLVMIRInterpreterError.runtime("Objective-C invocation is unavailable on this platform.")
#endif
    }

    private func nilObjcMessageResult(for returnType: LLVMFunctionReturnType) -> LLVMValue? {
        switch returnType {
        case .void:
            return nil
        case .type(.ptr):
            return .pointer(0)
        case .type(.i1):
            return .bool(false)
        case .type(.i8), .type(.i32), .type(.i64):
            return .int(0)
        }
    }

    private func scalarFromValue(_ value: LLVMValue) throws -> Double {
        switch value {
        case let .int(v):
            return Double(v)
        case let .bool(v):
            return v ? 1.0 : 0.0
        default:
            throw LLVMIRInterpreterError.runtime("Expected scalar numeric argument.")
        }
    }
}

private nonisolated struct LLVMModule {
    let functions: [String: LLVMFunction]
    let hostGlobals: [Int: LLVMHostGlobal]
}

private nonisolated enum LLVMHostGlobal: Equatable, Sendable {
    case selector(name: String, symbol: String)
    case classReference(name: String, symbol: String)
}

private nonisolated struct LLVMHostHandle: Equatable, Hashable, Sendable {
    enum Kind: UInt8, Sendable {
        case object
        case classReference
        case selector
        case nativeSymbol
    }

    let address: Int
    let kind: Kind
    let symbol: String
}

// Each instance is confined to one synchronous interpreter invocation.
private nonisolated final class LLVMExecutionBudget: @unchecked Sendable {
    private static let stepLimit = 200_000
    private static let callDepthLimit = 4

    private var stepCount = 0
    private var callDepth = 0

    func consumeStep() throws {
        stepCount += 1
        guard stepCount <= Self.stepLimit else {
            throw LLVMIRInterpreterError.runtime("Step limit exceeded. Possible infinite loop.")
        }
    }

    func enterFunction() throws {
        guard callDepth < Self.callDepthLimit else {
            throw LLVMIRInterpreterError.runtime("Call depth limit exceeded.")
        }
        callDepth += 1
    }

    func leaveFunction() {
        callDepth -= 1
    }
}

// Each instance is confined to one synchronous interpreter invocation.
private nonisolated final class LLVMRuntimeState: @unchecked Sendable {
    var memory: [Int: LLVMValue] = [:]
    var nextAddress = 1
    private let hostGlobals: [Int: LLVMHostGlobal]
    private var nextExternalAddress = 1_000_000
    private var derivedPointerTable: [String: Int] = [:]
    private var nextObjectAddress = 3_000_000
    private var handleByAddress: [Int: LLVMHostHandle] = [:]
    private var objectByAddress: [Int: AnyObject] = [:]
    private var addressByObjectID: [ObjectIdentifier: Int] = [:]

    init(hostGlobals: [Int: LLVMHostGlobal]) {
        self.hostGlobals = hostGlobals
    }

    func allocateExternalPointer() -> Int {
        let pointer = nextExternalAddress
        nextExternalAddress += 1
        return pointer
    }

    func derivedPointer(base: Int, indices: [Int]) -> Int {
        let key = "\(base)|\(indices.map(String.init).joined(separator: ","))"
        if let existing = derivedPointerTable[key] {
            return existing
        }
        let address = nextAddress
        nextAddress += 1
        derivedPointerTable[key] = address
        return address
    }

    func registerObject(
        _ object: AnyObject,
        kind: LLVMHostHandle.Kind = .object,
        symbol: String = ""
    ) -> LLVMHostHandle {
        let id = ObjectIdentifier(object)
        if let existing = addressByObjectID[id] {
            if let handle = handleByAddress[existing] {
                return handle
            }
            let handle = LLVMHostHandle(address: existing, kind: kind, symbol: symbol)
            handleByAddress[existing] = handle
            return handle
        }
        let address = nextObjectAddress
        nextObjectAddress += 1
        let handle = LLVMHostHandle(address: address, kind: kind, symbol: symbol)
        addressByObjectID[id] = address
        objectByAddress[address] = object
        handleByAddress[address] = handle
        return handle
    }

    func object(for value: LLVMValue) -> AnyObject? {
        switch value {
        case let .hostHandle(handle):
            return objectByAddress[handle.address]
        case let .pointer(address):
            guard address != 0 else { return nil }
            return objectByAddress[address]
        default:
            return nil
        }
    }

    func pointerValue(for address: Int) -> LLVMValue {
        if let handle = handleByAddress[address] {
            return .hostHandle(handle)
        }
        return .pointer(address)
    }

    func loadHostGlobal(at address: Int) throws -> LLVMValue? {
        guard let global = hostGlobals[address] else {
            return nil
        }
        switch global {
        case let .selector(name, _):
            let handle = LLVMHostHandle(address: address, kind: .selector, symbol: name)
            handleByAddress[address] = handle
            return .hostHandle(handle)
        case let .classReference(name, _):
            return try classReference(named: name)
        }
    }

    func classReference(named name: String) throws -> LLVMValue {
        let pointer = name.withCString { IRHFObjCLookUpClass($0) }
        guard let pointer else {
            throw LLVMIRInterpreterError.runtime("Objective-C class \(name) was not found.")
        }
        let object = Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue()
        return .hostHandle(registerObject(object, kind: .classReference, symbol: name))
    }

    func selectorName(for value: LLVMValue) -> String? {
        guard case let .hostHandle(handle) = value,
              handle.kind == .selector else {
            return nil
        }
        return handle.symbol
    }

    func objcBridgeValue(for value: LLVMValue) throws -> IRHFValue {
        var bridge = IRHFValue()
        bridge.bytes = nil
        bridge.byteCount = 0

        switch value {
        case let .int(integer):
            bridge.kind = IRHFValueKind(rawValue: 1)
            bridge.bits = UInt64(bitPattern: Int64(integer))
        case let .bool(boolean):
            bridge.kind = IRHFValueKind(rawValue: 3)
            bridge.bits = boolean ? 1 : 0
        case let .pointer(address):
            bridge.kind = IRHFValueKind(rawValue: 6)
            bridge.bits = UInt64(UInt(bitPattern: address))
        case let .hostHandle(handle):
            switch handle.kind {
            case .object, .classReference:
                guard let object = objectByAddress[handle.address] else {
                    throw LLVMIRInterpreterError.runtime("Host object handle is no longer live.")
                }
                bridge.kind = IRHFValueKind(rawValue: 7)
                bridge.bits = UInt64(UInt(bitPattern: Unmanaged.passUnretained(object).toOpaque()))
            case .selector:
                let pointer = handle.symbol.withCString { IRHFObjCRegisterSelector($0) }
                bridge.kind = IRHFValueKind(rawValue: 6)
                bridge.bits = UInt64(UInt(bitPattern: pointer))
            case .nativeSymbol:
                bridge.kind = IRHFValueKind(rawValue: 6)
                bridge.bits = UInt64(UInt(bitPattern: handle.address))
            }
        case .aggregate:
            throw LLVMIRInterpreterError.runtime(
                "Objective-C aggregate arguments require byte-layout lowering."
            )
        }
        return bridge
    }

    func value(from bridge: IRHFValue) throws -> LLVMValue? {
        switch bridge.kind.rawValue {
        case 9:
            return nil
        case 1, 2:
            return .int(Int(bitPattern: UInt(bridge.bits)))
        case 3:
            return .bool(bridge.bits != 0)
        case 6:
            return pointerValue(for: Int(bitPattern: UInt(bridge.bits)))
        case 7:
            guard bridge.bits != 0,
                  let pointer = UnsafeRawPointer(bitPattern: UInt(bridge.bits)) else {
                return .pointer(0)
            }
            let object = Unmanaged<AnyObject>.fromOpaque(pointer).takeRetainedValue()
            return .hostHandle(registerObject(object))
        case 4, 5:
            throw LLVMIRInterpreterError.runtime(
                "Floating-point Objective-C results are not represented by the current LLVMValue model."
            )
        case 8:
            throw LLVMIRInterpreterError.runtime(
                "Aggregate Objective-C results require byte-layout lowering."
            )
        default:
            throw LLVMIRInterpreterError.runtime("Objective-C bridge returned an invalid value kind.")
        }
    }
}

private nonisolated struct LLVMFunction {
    let name: String
    let returnType: LLVMFunctionReturnType
    let parameters: [LLVMFunctionParameter]
    let blocks: [LLVMBasicBlock]
}

private nonisolated struct LLVMFunctionParameter {
    let name: String
    let type: LLVMType
}

private nonisolated struct LLVMBasicBlock {
    let label: String
    let instructions: [LLVMInstruction]
}

private nonisolated struct LLVMPhiIncoming {
    let value: LLVMOperand
    let label: String
}

private nonisolated enum LLVMInstruction {
    case add(result: String, lhs: LLVMOperand, rhs: LLVMOperand)
    case sub(result: String, lhs: LLVMOperand, rhs: LLVMOperand)
    case mul(result: String, lhs: LLVMOperand, rhs: LLVMOperand)
    case sdiv(result: String, lhs: LLVMOperand, rhs: LLVMOperand)
    case and(result: String, type: LLVMType, lhs: LLVMOperand, rhs: LLVMOperand)
    case or(result: String, type: LLVMType, lhs: LLVMOperand, rhs: LLVMOperand)
    case xor(result: String, type: LLVMType, lhs: LLVMOperand, rhs: LLVMOperand)
    case shl(result: String, type: LLVMType, lhs: LLVMOperand, rhs: LLVMOperand)
    case ashr(result: String, type: LLVMType, lhs: LLVMOperand, rhs: LLVMOperand)
    case zext(result: String, from: LLVMType, operand: LLVMOperand, to: LLVMType)
    case sext(result: String, from: LLVMType, operand: LLVMOperand, to: LLVMType)
    case trunc(result: String, from: LLVMType, operand: LLVMOperand, to: LLVMType)
    case ptrtoint(result: String, value: LLVMOperand, to: LLVMType)
    case inttoptr(result: String, value: LLVMOperand, to: LLVMType)
    case extractvalue(result: String, aggregate: LLVMOperand, index: Int)
    case insertvalue(result: String, aggregate: LLVMOperand, element: LLVMOperand, index: Int)
    case getelementptr(result: String, base: LLVMOperand, indices: [LLVMOperand])
    case icmp(result: String, type: LLVMType, predicate: LLVMICmpPredicate, lhs: LLVMOperand, rhs: LLVMOperand)
    case phi(result: String, incomings: [LLVMPhiIncoming])
    case alloca(result: String, allocatedType: LLVMType)
    case store(value: LLVMOperand, pointer: LLVMOperand)
    case load(result: String, loadedType: LLVMType, pointer: LLVMOperand)
    case call(result: String?, functionName: String, arguments: [LLVMOperand], returnType: LLVMFunctionReturnType)
    case inlineAsm(result: String?, returnType: LLVMFunctionReturnType)
    case br(label: String)
    case brCond(cond: LLVMOperand, trueLabel: String, falseLabel: String)
    case ret(LLVMOperand)
    case retVoid
    case unreachable
}

private nonisolated enum LLVMType {
    case i8
    case i32
    case i64
    case i1
    case ptr
}

private nonisolated enum LLVMICmpPredicate: String {
    case eq
    case ne
    case slt
    case sle
    case sgt
    case sge
}

private nonisolated enum LLVMOperand {
    case int(Int)
    case bool(Bool)
    case pointer(Int)
    case undef
    case local(String)
}

private nonisolated enum LLVMValue: Sendable {
    case int(Int)
    case bool(Bool)
    case pointer(Int)
    case hostHandle(LLVMHostHandle)
    case aggregate([LLVMValue])

    func matches(type: LLVMType) -> Bool {
        switch (self, type) {
        case (.int, .i8), (.int, .i32), (.int, .i64), (.bool, .i1),
             (.pointer, .ptr), (.hostHandle, .ptr):
            return true
        default:
            return false
        }
    }
}

private nonisolated enum LLVMFunctionReturnType {
    case void
    case type(LLVMType)

    func matches(value: LLVMValue) -> Bool {
        switch self {
        case .void:
            return false
        case let .type(type):
            return value.matches(type: type)
        }
    }

    func isCompatible(with other: LLVMFunctionReturnType) -> Bool {
        switch (self, other) {
        case (.void, .void),
             (.type(.i8), .type(.i8)),
             (.type(.i32), .type(.i32)),
             (.type(.i64), .type(.i64)),
             (.type(.i1), .type(.i1)),
             (.type(.ptr), .type(.ptr)):
            return true
        default:
            return false
        }
    }
}

private nonisolated struct LLVMIRParser {
    func parseModule(ir: String) throws -> LLVMModule {
        let lines = sanitizeLines(ir)
        let hostGlobals = try parseHostGlobals(lines: lines)
        var index = 0
        var functions: [String: LLVMFunction] = [:]

        while index < lines.count {
            let line = lines[index]
            guard line.hasPrefix("define ") else {
                index += 1
                continue
            }

            let functionHeader = try parseFunctionHeader(header: line)
            index += 1

            var blocks: [LLVMBasicBlock] = []
            var currentLabel: String?
            var currentInstructions: [LLVMInstruction] = []

            while index < lines.count, lines[index] != "}" {
                let bodyLine = lines[index]
                if bodyLine.hasSuffix(":") {
                    if let label = currentLabel {
                        blocks.append(LLVMBasicBlock(label: label, instructions: currentInstructions))
                    }
                    currentLabel = String(bodyLine.dropLast())
                    currentInstructions = []
                } else if !bodyLine.isEmpty {
                    let instruction = try parseInstruction(line: bodyLine)
                    currentInstructions.append(instruction)
                }
                index += 1
            }

            if let label = currentLabel {
                blocks.append(LLVMBasicBlock(label: label, instructions: currentInstructions))
            }

            if index >= lines.count || lines[index] != "}" {
                throw LLVMIRInterpreterError.parse("Function @\(functionHeader.name) missing closing brace.")
            }
            index += 1

            if blocks.isEmpty {
                throw LLVMIRInterpreterError.parse("Function @\(functionHeader.name) has no basic blocks.")
            }
            var blockLabels = Set<String>()
            for block in blocks {
                guard blockLabels.insert(block.label).inserted else {
                    throw LLVMIRInterpreterError.parse(
                        "Duplicate basic block label %\(block.label) in function @\(functionHeader.name)."
                    )
                }
            }
            functions[functionHeader.name] = LLVMFunction(
                name: functionHeader.name,
                returnType: functionHeader.returnType,
                parameters: functionHeader.parameters,
                blocks: blocks
            )
        }

        return LLVMModule(functions: functions, hostGlobals: hostGlobals)
    }

    private func sanitizeLines(_ ir: String) -> [String] {
        ir
            .components(separatedBy: .newlines)
            .map { raw in
                var line = raw
                if let comment = line.firstIndex(of: ";") {
                    line = String(line[..<comment])
                }
                return line.trimmingCharacters(in: .whitespacesAndNewlines)
            }
            .filter { !$0.isEmpty }
    }

    private func parseHostGlobals(lines: [String]) throws -> [Int: LLVMHostGlobal] {
        var globals: [Int: LLVMHostGlobal] = [:]
        for line in lines {
            for symbol in globalSymbolTokens(in: line) {
                guard let global = parseHostGlobal(symbol: symbol) else {
                    continue
                }
                let address = llvmStableSymbolAddress(symbol)
                if let existing = globals[address], existing != global {
                    throw LLVMIRInterpreterError.parse(
                        "Host global address collision between \(existing) and \(global)."
                    )
                }
                globals[address] = global
            }
        }
        return globals
    }

    private func parseHostGlobal(symbol: String) -> LLVMHostGlobal? {
        if let marker = symbol.range(of: "L_selector(") {
            let suffix = symbol[marker.upperBound...]
            guard let close = suffix.firstIndex(of: ")") else {
                return nil
            }
            let name = String(suffix[..<close])
            guard !name.isEmpty else { return nil }
            return .selector(name: name, symbol: symbol)
        }

        for marker in ["OBJC_CLASS_REF_$_", "OBJC_CLASS_$_"] {
            guard let range = symbol.range(of: marker) else { continue }
            let suffix = symbol[range.upperBound...]
            let name = String(suffix.prefix { character in
                character != "\"" && character != "\\"
            })
            guard !name.isEmpty else { return nil }
            return .classReference(name: name, symbol: symbol)
        }
        return nil
    }

    private func globalSymbolTokens(in line: String) -> [String] {
        var tokens: [String] = []
        var cursor = line.startIndex

        while cursor < line.endIndex,
              let atSign = line[cursor...].firstIndex(of: "@") {
            let afterAt = line.index(after: atSign)
            guard afterAt < line.endIndex else { break }

            if line[afterAt] == "\"" {
                let bodyStart = line.index(after: afterAt)
                guard let closingQuote = line[bodyStart...].firstIndex(of: "\"") else {
                    break
                }
                let tokenEnd = line.index(after: closingQuote)
                tokens.append(String(line[atSign..<tokenEnd]))
                cursor = tokenEnd
                continue
            }

            var tokenEnd = afterAt
            while tokenEnd < line.endIndex {
                let character = line[tokenEnd]
                if character.isWhitespace || ",()[]{}=".contains(character) {
                    break
                }
                tokenEnd = line.index(after: tokenEnd)
            }
            if tokenEnd > afterAt {
                tokens.append(String(line[atSign..<tokenEnd]))
            }
            cursor = tokenEnd > atSign ? tokenEnd : afterAt
        }
        return tokens
    }

    private func parseFunctionHeader(header: String) throws -> (name: String, returnType: LLVMFunctionReturnType, parameters: [LLVMFunctionParameter]) {
        guard header.hasPrefix("define ") else {
            throw LLVMIRInterpreterError.parse("Invalid function header: \(header)")
        }

        let payload = String(header.dropFirst("define ".count))
        guard let atSign = payload.firstIndex(of: "@"),
              let openParen = payload[atSign...].firstIndex(of: "("),
              let closeParen = payload[openParen...].firstIndex(of: ")") else {
            throw LLVMIRInterpreterError.parse("Invalid function header: \(header)")
        }

        let returnPrefix = String(payload[..<atSign]).trimmingCharacters(in: .whitespaces)
        let returnType = try parseReturnTypeFromPrefix(returnPrefix, context: header)

        let nameStart = payload.index(after: atSign)
        let rawName = String(payload[nameStart..<openParen]).trimmingCharacters(in: .whitespaces)
        let name = normalizeFunctionName(rawName)
        if name.isEmpty {
            throw LLVMIRInterpreterError.parse("Invalid function name in header: \(header)")
        }

        let parametersText = String(payload[payload.index(after: openParen)..<closeParen]).trimmingCharacters(in: .whitespaces)
        let parameters = try parseFunctionParameters(parametersText)
        return (name: name, returnType: returnType, parameters: parameters)
    }

    private func parseReturnTypeFromPrefix(_ prefix: String, context: String) throws -> LLVMFunctionReturnType {
        let tokens = prefix.split(whereSeparator: \.isWhitespace).map(String.init)
        for token in tokens.reversed() {
            if let returnType = try? parseFunctionReturnType(token) {
                return returnType
            }
        }
        throw LLVMIRInterpreterError.parse("Cannot infer function return type: \(context)")
    }

    private func parseInstruction(line: String) throws -> LLVMInstruction {
        if line == "unreachable" {
            return .unreachable
        }
        if line.hasPrefix("store ") {
            return try parseStore(line: line)
        }
        if line.hasPrefix("call ") {
            if line.contains(" asm ") {
                return try parseInlineAsmCall(line: line, result: nil)
            }
            return try parseCall(line: line, result: nil)
        }
        if line.hasPrefix("br i1 ") {
            return try parseBrCond(line: line)
        }
        if line.hasPrefix("br label ") {
            return try parseBr(line: line)
        }
        if line.hasPrefix("ret ") {
            return try parseRet(line: line)
        }

        guard let eqRange = line.range(of: "=") else {
            throw LLVMIRInterpreterError.parse("Unsupported instruction: \(line)")
        }
        let result = String(line[..<eqRange.lowerBound]).trimmingCharacters(in: .whitespaces)
        guard result.hasPrefix("%") else {
            throw LLVMIRInterpreterError.parse("Result must be a local SSA value: \(line)")
        }
        let resultName = String(result.dropFirst())
        let rhs = String(line[eqRange.upperBound...]).trimmingCharacters(in: .whitespaces)

        if rhs.hasPrefix("add i32 ") {
            let (lhs, rhsOp) = try parseIntOperands(prefix: "add i32 ", text: rhs)
            return .add(result: resultName, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("add i64 ") {
            let (lhs, rhsOp) = try parseIntOperands(prefix: "add i64 ", text: rhs)
            return .add(result: resultName, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("sub i32 ") {
            let (lhs, rhsOp) = try parseIntOperands(prefix: "sub i32 ", text: rhs)
            return .sub(result: resultName, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("mul i32 ") {
            let (lhs, rhsOp) = try parseIntOperands(prefix: "mul i32 ", text: rhs)
            return .mul(result: resultName, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("sdiv i32 ") {
            let (lhs, rhsOp) = try parseIntOperands(prefix: "sdiv i32 ", text: rhs)
            return .sdiv(result: resultName, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("and ") {
            let (type, lhs, rhsOp) = try parseTypedBinaryOperands(prefix: "and ", text: rhs)
            return .and(result: resultName, type: type, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("or ") {
            let (type, lhs, rhsOp) = try parseTypedBinaryOperands(prefix: "or ", text: rhs)
            return .or(result: resultName, type: type, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("xor ") {
            let (type, lhs, rhsOp) = try parseTypedBinaryOperands(prefix: "xor ", text: rhs)
            return .xor(result: resultName, type: type, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("shl ") {
            let (type, lhs, rhsOp) = try parseTypedBinaryOperands(prefix: "shl ", text: rhs)
            return .shl(result: resultName, type: type, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("ashr ") {
            let (type, lhs, rhsOp) = try parseTypedBinaryOperands(prefix: "ashr ", text: rhs)
            return .ashr(result: resultName, type: type, lhs: lhs, rhs: rhsOp)
        }
        if rhs.hasPrefix("zext ") {
            let (from, operand, to) = try parseCastOperands(prefix: "zext ", text: rhs)
            return .zext(result: resultName, from: from, operand: operand, to: to)
        }
        if rhs.hasPrefix("sext ") {
            let (from, operand, to) = try parseCastOperands(prefix: "sext ", text: rhs)
            return .sext(result: resultName, from: from, operand: operand, to: to)
        }
        if rhs.hasPrefix("trunc ") {
            let (from, operand, to) = try parseCastOperands(prefix: "trunc ", text: rhs)
            return .trunc(result: resultName, from: from, operand: operand, to: to)
        }
        if rhs.hasPrefix("ptrtoint ") {
            let (value, toType) = try parsePtrToIntOperands(text: rhs)
            return .ptrtoint(result: resultName, value: value, to: toType)
        }
        if rhs.hasPrefix("inttoptr ") {
            let (value, toType) = try parseIntToPtrOperands(text: rhs)
            return .inttoptr(result: resultName, value: value, to: toType)
        }
        if rhs.hasPrefix("insertvalue ") {
            return try parseInsertValue(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("getelementptr ") || rhs.hasPrefix("getelementptr inbounds ") {
            return try parseGetElementPtr(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("extractvalue ") {
            return try parseExtractValue(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("icmp ") {
            return try parseICmp(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("phi ") {
            return try parsePhi(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("alloca ") {
            return try parseAlloca(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("load ") {
            return try parseLoad(result: resultName, rhs: rhs)
        }
        if rhs.hasPrefix("call ") {
            if rhs.contains(" asm ") {
                return try parseInlineAsmCall(line: rhs, result: resultName)
            }
            return try parseCall(line: rhs, result: resultName)
        }

        throw LLVMIRInterpreterError.parse("Unsupported instruction: \(line)")
    }

    private func parseAlloca(result: String, rhs: String) throws -> LLVMInstruction {
        let payload = String(rhs.dropFirst("alloca ".count))
        let typeToken = payload.split(separator: ",", maxSplits: 1).first.map(String.init)?
            .trimmingCharacters(in: .whitespaces) ?? payload
        let type = try parseType(typeToken)
        return .alloca(result: result, allocatedType: type)
    }

    private func parseStore(line: String) throws -> LLVMInstruction {
        let payload = String(line.dropFirst("store ".count))
        let parts = payload.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count >= 2 else {
            throw LLVMIRInterpreterError.parse("Invalid store instruction: \(line)")
        }
        let value = try parseTypedOperand(parts[0])
        let pointer = try parsePointerOperand(parts[1])
        return .store(value: value, pointer: pointer)
    }

    private func parseLoad(result: String, rhs: String) throws -> LLVMInstruction {
        let payload = String(rhs.dropFirst("load ".count))
        let parts = payload.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count >= 2 else {
            throw LLVMIRInterpreterError.parse("Invalid load instruction: \(rhs)")
        }
        let loadedType = try parseType(parts[0])
        let pointer = try parsePointerOperand(parts[1])
        return .load(result: result, loadedType: loadedType, pointer: pointer)
    }

    private func parseBrCond(line: String) throws -> LLVMInstruction {
        let payload = String(line.dropFirst("br i1 ".count))
        let parts = payload.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count == 3 else {
            throw LLVMIRInterpreterError.parse("Invalid conditional branch: \(line)")
        }
        let cond = try parseOperand(parts[0], as: .i1)
        guard parts[1].hasPrefix("label %"), parts[2].hasPrefix("label %") else {
            throw LLVMIRInterpreterError.parse("Invalid branch labels: \(line)")
        }
        let trueLabel = String(parts[1].dropFirst("label %".count))
        let falseLabel = String(parts[2].dropFirst("label %".count))
        return .brCond(cond: cond, trueLabel: trueLabel, falseLabel: falseLabel)
    }

    private func parseBr(line: String) throws -> LLVMInstruction {
        guard line.hasPrefix("br label %") else {
            throw LLVMIRInterpreterError.parse("Invalid branch: \(line)")
        }
        let label = String(line.dropFirst("br label %".count))
        return .br(label: label)
    }

    private func parseRet(line: String) throws -> LLVMInstruction {
        let payload = String(line.dropFirst("ret ".count))
        if payload == "void" {
            return .retVoid
        }
        let tokens = payload.split(whereSeparator: \.isWhitespace).map(String.init)
        guard tokens.count >= 2 else {
            throw LLVMIRInterpreterError.parse("Unsupported return type: \(line)")
        }
        let type = try parseType(tokens[0])
        let operandToken = extractOperandToken(from: String(payload.dropFirst(tokens[0].count)))
        return .ret(try parseOperand(operandToken, as: type))
    }

    private func parseCall(line: String, result: String?) throws -> LLVMInstruction {
        let payload = String(line.dropFirst("call ".count))
        guard let atSign = payload.firstIndex(of: "@") else {
            throw LLVMIRInterpreterError.parse("Invalid call instruction: \(line)")
        }

        let beforeAt = String(payload[..<atSign]).trimmingCharacters(in: .whitespaces)
        let returnType = try parseCallReturnType(beforeAt: beforeAt, line: line)
        let rest = String(payload[atSign...]).trimmingCharacters(in: .whitespaces)

        guard let openParen = rest.firstIndex(of: "("),
              let closeParen = rest[openParen...].lastIndex(of: ")") else {
            throw LLVMIRInterpreterError.parse("Invalid call target: \(line)")
        }

        let rawFunctionName = String(rest[..<openParen]).trimmingCharacters(in: .whitespaces)
        let functionName = normalizeFunctionName(rawFunctionName)
        if functionName.isEmpty {
            throw LLVMIRInterpreterError.parse("Invalid callee in call: \(line)")
        }

        let argsText = String(rest[rest.index(after: openParen)..<closeParen]).trimmingCharacters(in: .whitespaces)
        let arguments: [LLVMOperand]
        if argsText.isEmpty {
            arguments = []
        } else {
            let argParts = splitTopLevelComma(argsText)
            arguments = try argParts.map { try parseTypedOperand($0) }
        }

        return .call(result: result, functionName: functionName, arguments: arguments, returnType: returnType)
    }

    private func parseInlineAsmCall(line: String, result: String?) throws -> LLVMInstruction {
        let payload = String(line.dropFirst("call ".count))
        guard let asmRange = payload.range(of: " asm ") else {
            throw LLVMIRInterpreterError.parse("Invalid inline asm call: \(line)")
        }
        let prefix = String(payload[..<asmRange.lowerBound]).trimmingCharacters(in: .whitespaces)
        let returnType = try parseCallReturnType(beforeAt: prefix, line: line)
        return .inlineAsm(result: result, returnType: returnType)
    }

    private func parseExtractValue(result: String, rhs: String) throws -> LLVMInstruction {
        let payload = String(rhs.dropFirst("extractvalue ".count))
        guard let comma = payload.lastIndex(of: ",") else {
            throw LLVMIRInterpreterError.parse("Invalid extractvalue instruction: \(rhs)")
        }

        let left = String(payload[..<comma]).trimmingCharacters(in: .whitespaces)
        let right = String(payload[payload.index(after: comma)...]).trimmingCharacters(in: .whitespaces)
        guard let index = Int(right.split(whereSeparator: \.isWhitespace).first.map(String.init) ?? "") else {
            throw LLVMIRInterpreterError.parse("Invalid extractvalue index: \(rhs)")
        }
        // LLVM textual form is usually: `extractvalue <aggregate-ty> <aggregate-val>, <idx>`
        // We need the aggregate value token (e.g. `%1`), not the type token (e.g. `%swift.metadata_response`).
        let tokens = left.split(whereSeparator: \.isWhitespace).map(String.init)
        guard let valueToken = tokens.last else {
            throw LLVMIRInterpreterError.parse("Invalid extractvalue aggregate operand: \(rhs)")
        }
        let aggregate = try parseOperand(valueToken)
        return .extractvalue(result: result, aggregate: aggregate, index: index)
    }

    private func parseInsertValue(result: String, rhs: String) throws -> LLVMInstruction {
        let payload = String(rhs.dropFirst("insertvalue ".count))
        let parts = payload.split(separator: ",", maxSplits: 2).map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count == 3 else {
            throw LLVMIRInterpreterError.parse("Invalid insertvalue instruction: \(rhs)")
        }

        // part0: "<agg-ty> <agg-val>"
        // part1: "<elt-ty> <elt-val>"
        // part2: "<index>"
        let aggregateTokens = parts[0].split(whereSeparator: \.isWhitespace).map(String.init)
        guard aggregateTokens.count >= 2 else {
            throw LLVMIRInterpreterError.parse("Invalid insertvalue aggregate operand: \(rhs)")
        }
        let aggregateValueToken = aggregateTokens.last!
        let aggregate = aggregateValueToken == "undef" ? LLVMOperand.undef : try parseOperand(aggregateValueToken)

        let element = try parseTypedOperand(parts[1])
        guard let index = Int(parts[2].split(whereSeparator: \.isWhitespace).first.map(String.init) ?? "") else {
            throw LLVMIRInterpreterError.parse("Invalid insertvalue index: \(rhs)")
        }
        return .insertvalue(result: result, aggregate: aggregate, element: element, index: index)
    }

    private func parseGetElementPtr(result: String, rhs: String) throws -> LLVMInstruction {
        let payload: String
        if rhs.hasPrefix("getelementptr inbounds ") {
            payload = String(rhs.dropFirst("getelementptr inbounds ".count))
        } else {
            payload = String(rhs.dropFirst("getelementptr ".count))
        }

        let parts = payload.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count >= 2 else {
            throw LLVMIRInterpreterError.parse("Invalid getelementptr instruction: \(rhs)")
        }

        let base = try parsePointerOperand(parts[1])
        let indices: [LLVMOperand] = try parts.dropFirst(2).map { try parseTypedOperand($0) }
        return .getelementptr(result: result, base: base, indices: indices)
    }

    private func parseCallReturnType(beforeAt: String, line: String) throws -> LLVMFunctionReturnType {
        let tokens = beforeAt.split(whereSeparator: \.isWhitespace).map(String.init)
        for token in tokens.reversed() {
            if let returnType = try? parseFunctionReturnType(token) {
                return returnType
            }
        }
        throw LLVMIRInterpreterError.parse("Cannot infer call return type: \(line)")
    }

    private func normalizeFunctionName(_ raw: String) -> String {
        var value = raw.trimmingCharacters(in: .whitespaces)
        if value.hasPrefix("@") {
            value.removeFirst()
        }
        if value.hasPrefix("\""), value.hasSuffix("\""), value.count >= 2 {
            value.removeFirst()
            value.removeLast()
        }
        return value
    }

    private func parsePtrToIntOperands(text: String) throws -> (LLVMOperand, LLVMType) {
        let payload = String(text.dropFirst("ptrtoint ".count))
        guard let toRange = payload.range(of: " to ") else {
            throw LLVMIRInterpreterError.parse("Invalid ptrtoint instruction: \(text)")
        }
        let lhs = String(payload[..<toRange.lowerBound]).trimmingCharacters(in: .whitespaces)
        let rhs = String(payload[toRange.upperBound...]).trimmingCharacters(in: .whitespaces)

        let pieces = lhs.split(separator: " ", maxSplits: 1).map(String.init)
        guard pieces.count == 2 else {
            throw LLVMIRInterpreterError.parse("Invalid ptrtoint operand: \(text)")
        }
        let fromType = try parseType(pieces[0])
        guard fromType == .ptr else {
            throw LLVMIRInterpreterError.parse("ptrtoint source must be ptr: \(text)")
        }
        let value = try parseOperand(pieces[1], as: .ptr)
        let toType = try parseType(rhs)
        guard toType == .i32 || toType == .i64 else {
            throw LLVMIRInterpreterError.parse("ptrtoint target must be i32 or i64: \(text)")
        }
        return (value, toType)
    }

    private func parseIntToPtrOperands(text: String) throws -> (LLVMOperand, LLVMType) {
        let payload = String(text.dropFirst("inttoptr ".count))
        guard let toRange = payload.range(of: " to ") else {
            throw LLVMIRInterpreterError.parse("Invalid inttoptr instruction: \(text)")
        }
        let lhs = String(payload[..<toRange.lowerBound]).trimmingCharacters(in: .whitespaces)
        let rhs = String(payload[toRange.upperBound...]).trimmingCharacters(in: .whitespaces)

        let pieces = lhs.split(separator: " ", maxSplits: 1).map(String.init)
        guard pieces.count == 2 else {
            throw LLVMIRInterpreterError.parse("Invalid inttoptr operand: \(text)")
        }
        let fromType = try parseType(pieces[0])
        guard fromType == .i32 || fromType == .i64 else {
            throw LLVMIRInterpreterError.parse("inttoptr source must be i32 or i64: \(text)")
        }
        let value = try parseOperand(pieces[1], as: fromType)
        let toType = try parseType(rhs)
        guard toType == .ptr else {
            throw LLVMIRInterpreterError.parse("inttoptr target must be ptr: \(text)")
        }
        return (value, toType)
    }

    private func splitTopLevelComma(_ input: String) -> [String] {
        var parts: [String] = []
        var current = ""
        var parenDepth = 0
        var bracketDepth = 0
        var inQuotes = false

        for ch in input {
            if ch == "\"" {
                inQuotes.toggle()
                current.append(ch)
                continue
            }
            if !inQuotes {
                if ch == "(" { parenDepth += 1 }
                if ch == ")" { parenDepth = max(0, parenDepth - 1) }
                if ch == "[" { bracketDepth += 1 }
                if ch == "]" { bracketDepth = max(0, bracketDepth - 1) }

                if ch == "," && parenDepth == 0 && bracketDepth == 0 {
                    parts.append(current.trimmingCharacters(in: .whitespaces))
                    current = ""
                    continue
                }
            }
            current.append(ch)
        }

        let tail = current.trimmingCharacters(in: .whitespaces)
        if !tail.isEmpty {
            parts.append(tail)
        }
        return parts
    }

    private func parseICmp(result: String, rhs: String) throws -> LLVMInstruction {
        let payload = String(rhs.dropFirst("icmp ".count))
        let parts = payload.split(separator: " ", maxSplits: 2).map(String.init)
        guard parts.count == 3 else {
            throw LLVMIRInterpreterError.parse("Invalid icmp: \(rhs)")
        }
        guard let predicate = LLVMICmpPredicate(rawValue: parts[0]) else {
            throw LLVMIRInterpreterError.parse("Unsupported icmp predicate: \(parts[0])")
        }
        let type = try parseType(parts[1])
        let operands = parts[2].split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard operands.count == 2 else {
            throw LLVMIRInterpreterError.parse("Invalid icmp operands: \(rhs)")
        }
        return .icmp(
            result: result,
            type: type,
            predicate: predicate,
            lhs: try parseOperand(operands[0], as: type),
            rhs: try parseOperand(operands[1], as: type)
        )
    }

    private func parsePhi(result: String, rhs: String) throws -> LLVMInstruction {
        let payload = String(rhs.dropFirst("phi ".count))
        guard let firstSpace = payload.firstIndex(of: " ") else {
            throw LLVMIRInterpreterError.parse("Invalid phi instruction: \(rhs)")
        }
        let typeToken = String(payload[..<firstSpace]).trimmingCharacters(in: .whitespaces)
        let type = try parseType(typeToken)
        let incomingText = String(payload[payload.index(after: firstSpace)...]).trimmingCharacters(in: .whitespaces)
        let incomingMatches = incomingText.matches(of: #/\[\s*([^,\]]+)\s*,\s*%([^\]\s]+)\s*\]/#)
        if incomingMatches.isEmpty {
            throw LLVMIRInterpreterError.parse("Invalid phi incoming list: \(rhs)")
        }
        let incomings: [LLVMPhiIncoming] = try incomingMatches.map { match in
            let valueText = String(match.1).trimmingCharacters(in: .whitespaces)
            let label = String(match.2).trimmingCharacters(in: .whitespaces)
            return LLVMPhiIncoming(value: try parseOperand(valueText, as: type), label: label)
        }
        return .phi(result: result, incomings: incomings)
    }

    private func parseIntOperands(prefix: String, text: String) throws -> (LLVMOperand, LLVMOperand) {
        let payload = String(text.dropFirst(prefix.count))
        let parts = payload.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count == 2 else {
            throw LLVMIRInterpreterError.parse("Invalid binary operands: \(text)")
        }
        return (try parseOperand(parts[0], as: .i32), try parseOperand(parts[1], as: .i32))
    }

    private func parseTypedBinaryOperands(prefix: String, text: String) throws -> (LLVMType, LLVMOperand, LLVMOperand) {
        let payload = String(text.dropFirst(prefix.count))
        guard let firstSpace = payload.firstIndex(of: " ") else {
            throw LLVMIRInterpreterError.parse("Invalid typed binary operands: \(text)")
        }

        let typeToken = String(payload[..<firstSpace]).trimmingCharacters(in: .whitespaces)
        let type = try parseType(typeToken)
        let rest = String(payload[payload.index(after: firstSpace)...]).trimmingCharacters(in: .whitespaces)
        let parts = rest.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard parts.count == 2 else {
            throw LLVMIRInterpreterError.parse("Invalid typed binary operands: \(text)")
        }
        return (type, try parseOperand(parts[0], as: type), try parseOperand(parts[1], as: type))
    }

    private func parseCastOperands(prefix: String, text: String) throws -> (LLVMType, LLVMOperand, LLVMType) {
        let payload = String(text.dropFirst(prefix.count))
        guard let toRange = payload.range(of: " to ") else {
            throw LLVMIRInterpreterError.parse("Invalid cast instruction: \(text)")
        }

        let left = String(payload[..<toRange.lowerBound]).trimmingCharacters(in: .whitespaces)
        let right = String(payload[toRange.upperBound...]).trimmingCharacters(in: .whitespaces)

        let pieces = left.split(separator: " ", maxSplits: 1).map(String.init)
        guard pieces.count == 2 else {
            throw LLVMIRInterpreterError.parse("Invalid cast source operands: \(text)")
        }

        let from = try parseType(pieces[0])
        let operand = try parseOperand(pieces[1].trimmingCharacters(in: .whitespaces), as: from)
        let to = try parseType(right)
        return (from, operand, to)
    }

    private func parseTypedOperand(_ raw: String) throws -> LLVMOperand {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        guard let firstSpace = trimmed.firstIndex(of: " ") else {
            throw LLVMIRInterpreterError.parse("Invalid typed operand: \(raw)")
        }
        let typeToken = String(trimmed[..<firstSpace])
        let type = try parseType(typeToken)
        let suffix = String(trimmed[firstSpace...]).trimmingCharacters(in: .whitespaces)
        let operandToken = extractOperandToken(from: suffix)
        return try parseOperand(operandToken, as: type)
    }

    private func parsePointerOperand(_ raw: String) throws -> LLVMOperand {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("ptr ") {
            return try parseOperand(String(trimmed.dropFirst("ptr ".count)), as: .ptr)
        }

        // Supports typed pointers like `i32* %p`.
        if let space = trimmed.firstIndex(of: " ") {
            let typePrefix = String(trimmed[..<space])
            if typePrefix.hasSuffix("*") {
                let operandText = String(trimmed[trimmed.index(after: space)...]).trimmingCharacters(in: .whitespaces)
                return try parseOperand(operandText, as: .ptr)
            }
        }

        throw LLVMIRInterpreterError.parse("Invalid pointer operand: \(raw)")
    }

    private func parseType(_ raw: String) throws -> LLVMType {
        let token = raw.trimmingCharacters(in: .whitespaces)
        if token.hasPrefix("%") {
            return .ptr
        }
        switch token {
        case "i8":
            return .i8
        case "double":
            // Current interpreter does not model FP semantics; treat as integer-like opaque scalar.
            return .i64
        case "i32":
            return .i32
        case "i64":
            return .i64
        case "i1":
            return .i1
        case "ptr":
            return .ptr
        default:
            if token.hasSuffix("*") {
                return .ptr
            }
            throw LLVMIRInterpreterError.parse("Unsupported type: \(raw)")
        }
    }

    private func parseFunctionReturnType(_ raw: String) throws -> LLVMFunctionReturnType {
        let token = raw.trimmingCharacters(in: .whitespaces)
        if token == "void" {
            return .void
        }
        // Swift IR may use opaque aggregate return types; treat them as opaque pointers for now.
        if token.hasPrefix("%") {
            return .type(.ptr)
        }
        return .type(try parseType(token))
    }

    private func parseFunctionParameters(_ raw: String) throws -> [LLVMFunctionParameter] {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty {
            return []
        }

        return try trimmed.split(separator: ",").map { segment in
            let part = String(segment).trimmingCharacters(in: .whitespaces)
            let tokens = part.split(whereSeparator: \.isWhitespace).map(String.init)
            guard tokens.count >= 2 else {
                throw LLVMIRInterpreterError.parse("Invalid function parameter: \(part)")
            }
            let type = try parseType(tokens[0])
            let nameToken = tokens.last!.trimmingCharacters(in: .whitespaces)
            guard nameToken.hasPrefix("%") else {
                throw LLVMIRInterpreterError.parse("Function parameter must be local SSA value: \(part)")
            }

            let name = String(nameToken.dropFirst())
            if name.isEmpty {
                throw LLVMIRInterpreterError.parse("Function parameter has empty name: \(part)")
            }
            return LLVMFunctionParameter(name: name, type: type)
        }
    }

    private func parseOperand(_ raw: String) throws -> LLVMOperand {
        if raw == "undef" {
            return .undef
        }
        if raw.hasPrefix("%") {
            return .local(String(raw.dropFirst()))
        }
        if raw == "true" {
            return .bool(true)
        }
        if raw == "false" {
            return .bool(false)
        }
        if let value = Int(raw) {
            return .int(value)
        }
        throw LLVMIRInterpreterError.parse("Invalid operand: \(raw)")
    }

    private func parseOperand(_ raw: String, as type: LLVMType) throws -> LLVMOperand {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        if trimmed == "undef" {
            return .undef
        }
        if trimmed.hasPrefix("%") {
            return .local(String(trimmed.dropFirst()))
        }

        switch type {
        case .i8:
            guard let value = Int(trimmed) else {
                if trimmed.hasPrefix("ptrtoint") {
                    return .int(0)
                }
                if trimmed.hasPrefix("@") {
                    return .int(stableGlobalPointer(for: trimmed))
                }
                throw LLVMIRInterpreterError.parse("Invalid i8 operand: \(raw)")
            }
            return .int(value)
        case .i32:
            guard let value = Int(trimmed) else {
                if let floating = parseExactIntegerFallback(trimmed) { return .int(floating) }
                if trimmed.hasPrefix("ptrtoint") {
                    return .int(0)
                }
                if trimmed.hasPrefix("@") {
                    return .int(stableGlobalPointer(for: trimmed))
                }
                throw LLVMIRInterpreterError.parse("Invalid i32 operand: \(raw)")
            }
            return .int(value)
        case .i64:
            guard let value = Int(trimmed) else {
                if let floating = parseExactIntegerFallback(trimmed) { return .int(floating) }
                if trimmed.hasPrefix("ptrtoint") {
                    return .int(0)
                }
                if trimmed.hasPrefix("@") {
                    return .int(stableGlobalPointer(for: trimmed))
                }
                throw LLVMIRInterpreterError.parse("Invalid i64 operand: \(raw)")
            }
            return .int(value)
        case .i1:
            if trimmed == "true" || trimmed == "1" {
                return .bool(true)
            }
            if trimmed == "false" || trimmed == "0" {
                return .bool(false)
            }
            throw LLVMIRInterpreterError.parse("Invalid i1 operand: \(raw)")
        case .ptr:
            if trimmed == "null" {
                return .pointer(0)
            }
            if trimmed.hasPrefix("@") {
                return .pointer(stableGlobalPointer(for: trimmed))
            }
            throw LLVMIRInterpreterError.parse("Invalid ptr operand: \(raw)")
        }
    }

    private func parseExactIntegerFallback(_ raw: String) -> Int? {
        guard let floating = Double(raw),
              floating.isFinite,
              floating.rounded(.towardZero) == floating else {
            return nil
        }
        return Int(exactly: floating)
    }

    private func extractOperandToken(from raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("ptrtoint") || trimmed.hasPrefix("inttoptr") {
            return trimmed
        }
        if trimmed.hasPrefix("@\"") {
            return trimmed
        }
        if trimmed.hasPrefix("%"), let token = trimmed.split(whereSeparator: \.isWhitespace).first {
            return String(token)
        }
        if trimmed.hasPrefix("@"), let token = trimmed.split(whereSeparator: \.isWhitespace).first {
            return String(token)
        }
        if let percent = trimmed.firstIndex(of: "%") {
            let value = String(trimmed[percent...])
            if let token = value.split(whereSeparator: \.isWhitespace).first {
                return String(token)
            }
        }
        if let at = trimmed.firstIndex(of: "@") {
            let value = String(trimmed[at...])
            if let token = value.split(whereSeparator: \.isWhitespace).first {
                return String(token)
            }
        }
        if let literal = trimmed.split(whereSeparator: \.isWhitespace).first {
            return String(literal)
        }
        return trimmed
    }

    private func stableGlobalPointer(for symbol: String) -> Int {
        var hash = 5381
        for scalar in symbol.unicodeScalars {
            hash = ((hash << 5) &+ hash) &+ Int(scalar.value)
        }
        return 2_000_000 + abs(hash % 1_000_000)
    }
}
