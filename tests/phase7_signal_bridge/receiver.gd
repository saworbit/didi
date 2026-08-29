@tool
extends Node

func _emitter() -> Node:
	return get_node("../ValueEmitter")

func _mark(signal_name: StringName) -> void:
	var emitter := _emitter()
	var callback := Callable(self, "marker")
	if not emitter.is_connected(signal_name, callback):
		emitter.connect(signal_name, callback, Object.CONNECT_PERSIST)

func receive_basic() -> void:
	pass

func receive_bulk() -> void:
	pass

func marker() -> void:
	pass

func receive_bool(value: bool) -> void:
	if typeof(value) == TYPE_BOOL and value:
		_mark(&"marker_bool")

func receive_int(value: int) -> void:
	if typeof(value) == TYPE_INT and value == 9:
		_mark(&"marker_int")

func receive_float(value: float) -> void:
	if typeof(value) == TYPE_FLOAT and value == 7.0:
		_mark(&"marker_float")

func receive_string(value: String) -> void:
	if typeof(value) == TYPE_STRING and value == "utf8-✓":
		_mark(&"marker_string")

func receive_array(value: Array) -> void:
	if value == [1, {"nested": true}, null]:
		_mark(&"marker_array")

func receive_dictionary(value: Dictionary) -> void:
	if value == {"alpha": [1, 2], "null": null}:
		_mark(&"marker_dictionary")

func receive_null(value: Node) -> void:
	if value == null:
		_mark(&"marker_null")
