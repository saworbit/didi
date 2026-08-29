@tool
extends Node

var configured := false

func _ready() -> void:
	call_deferred("_configure")

func _configure() -> void:
	if configured or not Engine.is_editor_hint():
		return
	configured = true
	var receiver := $Receiver
	var basic := $BasicEmitter
	var values := $ValueEmitter
	var bulk := $BulkEmitter

	basic.connect(&"unsupported", Callable(receiver, "receive_basic"), Object.CONNECT_DEFERRED)
	$InvalidFlagsEmitter.connect(&"invalid_flags", Callable(receiver, "receive_basic"), 16)

	values.connect(&"bool_value", Callable(receiver, "receive_bool"), Object.CONNECT_PERSIST)
	values.connect(&"int_value", Callable(receiver, "receive_int"), Object.CONNECT_PERSIST)
	values.connect(&"float_value", Callable(receiver, "receive_float"), Object.CONNECT_PERSIST)
	values.connect(&"string_value", Callable(receiver, "receive_string"), Object.CONNECT_PERSIST)
	values.connect(&"array_value", Callable(receiver, "receive_array"), Object.CONNECT_PERSIST)
	values.connect(&"dictionary_value", Callable(receiver, "receive_dictionary"), Object.CONNECT_PERSIST)
	values.connect(&"nullable_node", Callable(receiver, "receive_null"), Object.CONNECT_PERSIST)

	for index in range(299, -1, -1):
		var child := Node.new()
		child.name = "Receiver_%03d" % index
		child.set_script(load("res://receiver.gd"))
		$BulkReceivers.add_child(child)
		child.owner = self
		bulk.connect(&"a_bulk", Callable(child, "receive_bulk"), Object.CONNECT_PERSIST)

	for name in [&"éclair", &"zeta", &"alpha", &"Ångstrom"]:
		$OrderingEmitter.add_user_signal(name)
	for index in range(299, -1, -1):
		$SignalCapEmitter.add_user_signal("signal_%03d" % index)
	for index in range(1024, -1, -1):
		$OverflowEmitter.add_user_signal("overflow_%04d" % index)

	print("PHASE7_SIGNAL_FIXTURE_READY")
