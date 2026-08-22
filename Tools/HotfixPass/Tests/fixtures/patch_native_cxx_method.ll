define swiftcc i64 @instanceTarget(ptr swiftself %self, i64 %value) {
entry:
  %result = call i64 @_ZN16ReleasedCounter8multiplyEl(ptr %self, i64 %value)
  ret i64 %result
}

declare i64 @_ZN16ReleasedCounter8multiplyEl(ptr, i64) #0

attributes #0 = { "irhotfix.receiver-index"="0" }
