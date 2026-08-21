func hotfixFixture(_ value: Int) -> Int { value * 2 }

public final class HotfixReceiverFixture {
  public init() {}

  @inline(never)
  public final func instanceTarget(_ value: Int) -> Int { value + 3 }

  @inline(never)
  public static func staticTarget(_ value: Int) -> Int { value + 4 }

  @inline(never)
  public final func actorArgumentTarget(_ actor: any Actor) -> Int { 9 }
}

public struct HotfixValueFixture {
  private var stored = 0

  public init() {}

  @inline(never)
  public mutating func mutatingTarget(_ value: Int) -> Int {
    stored += value
    return stored
  }
}

public enum HotfixEnumFixture {
  case value

  @inline(never)
  public mutating func enumTarget(_ value: Int) -> Int { value + 5 }
}

public actor HotfixActorFixture {
  @inline(never)
  public func actorTarget(_ value: Int) -> Int { value + 6 }
}

public protocol HotfixProtocolFixture {
  func protocolTarget(_ value: Int) -> Int
}

public extension HotfixProtocolFixture {
  @inline(never)
  func protocolTarget(_ value: Int) -> Int { value + 7 }
}

@inline(never)
public func classArgumentTarget(_ receiver: HotfixReceiverFixture) -> Int { 8 }

@inline(never)
public func classReturnTarget() -> HotfixReceiverFixture {
  HotfixReceiverFixture()
}
