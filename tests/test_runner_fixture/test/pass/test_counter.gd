extends "res://addons/gut/test.gd"

const Counter = preload("res://src/counter.gd")


func test_starts_at_zero() -> void:
	assert_eq(Counter.new().value, 0)


func test_bumps_by_one() -> void:
	var counter = Counter.new()
	counter.bump()
	assert_eq(counter.bump(), 2)
