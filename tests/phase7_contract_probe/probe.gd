extends SceneTree

class ProbeEmitter:
    extends Node
    signal observed(value: int)

class ProbeReceiver:
    extends RefCounted
    var total := 0
    func receive(value: int) -> void:
        total += value

func require(condition: bool, message: String) -> void:
    if not condition:
        push_error(message)
        quit(1)

func _initialize() -> void:
    var emitter := ProbeEmitter.new()
    root.add_child(emitter)
    var receiver := ProbeReceiver.new()
    var callable := Callable(receiver, "receive")

    require(emitter.connect("observed", callable, 2) == OK, "flag 2 connect failed")
    var connections := emitter.get_signal_connection_list("observed")
    require(connections.size() == 1 and int(connections[0]["flags"]) == 2,
        "flag 2 was not observed")
    require(emitter.connect("observed", callable, 2) != OK,
        "duplicate connect unexpectedly succeeded")
    emitter.disconnect("observed", callable)
    require(not emitter.is_connected("observed", callable), "disconnect failed")
    require(emitter.connect("observed", callable, 2) == OK, "reconnect failed")
    emitter.disconnect("observed", callable)

    var history := UndoRedo.new()
    history.create_action("phase7 signal flag contract")
    history.add_do_method(Callable(emitter, "connect").bind("observed", callable, 2))
    history.add_undo_method(Callable(emitter, "disconnect").bind("observed", callable))
    history.commit_action()
    require(emitter.is_connected("observed", callable), "UndoRedo apply failed")
    history.undo()
    require(not emitter.is_connected("observed", callable), "UndoRedo undo failed")
    history.redo()
    require(emitter.is_connected("observed", callable), "UndoRedo redo failed")

    var key_identity_combinations := 0
    for mask in range(1, 8):
        var event := InputEventKey.new()
        if mask & 4:
            event.unicode = 65
        if mask & 2:
            event.physical_keycode = 66
        if mask & 1:
            event.keycode = 67
        require(event.unicode == (65 if mask & 4 else 0), "unicode identity drift")
        require(event.physical_keycode == (66 if mask & 2 else 0),
            "physical key identity drift")
        require(event.keycode == (67 if mask & 1 else 0), "keycode identity drift")
        key_identity_combinations += 1

    print("PHASE7_CONTRACT|signal_flag_combinations=1|key_identity_combinations=%d" %
        key_identity_combinations)
    quit(0)
