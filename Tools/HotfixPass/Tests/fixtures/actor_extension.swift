import ActorLib

public extension ExternalActor {
  @inline(never)
  func externalActorTarget(_ value: Int) -> Int { value + 10 }
}
