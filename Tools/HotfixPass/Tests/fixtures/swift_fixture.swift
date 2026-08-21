func hotfixFixture(_ value: Int) -> Int { value * 2 }

public final class HotfixReceiverFixture {
  public init() {}

  @inline(never)
  public final func instanceTarget(_ value: Int) -> Int { value + 3 }
}
