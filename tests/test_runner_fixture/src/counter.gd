extends RefCounted

# The code under test. The block edits STEP to show a recorded pass go stale.

const STEP := 1

var value := 0


func bump() -> int:
	value += STEP
	return value
