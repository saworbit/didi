extends RefCounted

# What a test case extends in the stand-in framework. GUT's message for
# assert_eq is "[got] expected to equal [expected]: text".

var failure := ""


func assert_eq(got, expected, text := "") -> void:
	if got != expected and failure == "":
		failure = "[%s] expected to equal [%s]: %s" % [str(got), str(expected), text]
