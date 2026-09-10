@tool
extends RefCounted

## Waits for a coroutine that `scene_call_method` started.
##
## A GDScript function containing `await` hands back a `GDScriptFunctionState`
## rather than its return value, and that value arrives on the state's
## `completed` signal. The extension cannot receive it: a custom `Callable`
## built through `callable_custom_create2` is constructed correctly and can be
## invoked directly, but `completed` never reaches it, and a normal `Callable`
## needs an Object with a registered method, which the extension has no reason
## to register.
##
## `await` is the mechanism the language already provides for exactly this, so
## the wait lives here and the extension reads two plain properties. Nothing
## here is called by a person; `scene_call_method` owns this file.

## Whether the coroutine has finished. The extension polls this once per frame.
var finished := false

## What the coroutine returned. Meaningless until `finished` is true.
var value = null

## Whether `watch` was ever able to start. A state that is not awaitable would
## otherwise leave `finished` false forever and look like a slow coroutine.
var started := false


func watch(state) -> void:
	started = true
	value = await state
	finished = true
