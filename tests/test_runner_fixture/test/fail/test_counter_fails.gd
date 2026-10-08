extends "res://addons/gut/test.gd"

const Counter = preload("res://src/counter.gd")


func test_bumps_once() -> void:
	assert_eq(Counter.new().bump(), 1)


func test_is_wrong_on_purpose() -> void:
	assert_eq(Counter.new().bump(), 3, "one bump is not three")
