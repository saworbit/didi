@tool
extends Node

signal a_bulk
signal basic
signal disconnect_case
signal mismatch_connect
signal mismatch_disconnect
signal unsupported
signal invalid_flags

signal bool_value(value: bool)
signal int_value(value: int)
signal float_value(value: float)
signal float_negative_value(value: float)
signal float_preflight_value(value: float)
signal string_value(value: String)
signal array_value(value: Array)
signal dictionary_value(value: Dictionary)
signal nullable_node(value: Node)
signal typed_array_value(value: Array[int])
signal typed_dictionary_value(value: Dictionary[String, int])
signal callable_value(value: Callable)
signal resource_value(value: Resource)

signal marker_bool
signal marker_int
signal marker_float
signal marker_float_negative
signal marker_float_preflight
signal marker_string
signal marker_array
signal marker_dictionary
signal marker_null
