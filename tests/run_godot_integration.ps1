param(
    [Parameter(Mandatory = $true)]
    [string]$GodotExecutable,
    [string]$Configuration = "Release",
    [int]$StartupTimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureRoot = Join-Path $PSScriptRoot "godot_smoke"
$didiExecutable = Join-Path $repoRoot "build\$Configuration\didi.exe"
$stdoutPath = Join-Path $repoRoot "build\godot_integration.out"
$stderrPath = Join-Path $repoRoot "build\godot_integration.err"

if (-not (Test-Path -LiteralPath $GodotExecutable)) {
    throw "Godot executable not found: $GodotExecutable"
}
if (-not (Test-Path -LiteralPath $didiExecutable)) {
    throw "Didi executable not found: $didiExecutable"
}

Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
$godot = $null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Tool-Request([int]$Id, [string]$Name, [hashtable]$Arguments) {
    return @{
        jsonrpc = "2.0"
        id = $Id
        method = "tools/call"
        params = @{ name = $Name; arguments = $Arguments }
    } | ConvertTo-Json -Compress -Depth 20
}

function Tool-Payload($Response) {
    Assert-True (-not $Response.result.isError) "Tool request $($Response.id) failed: $($Response.result.content[0].text)"
    return $Response.result.content[0].text | ConvertFrom-Json -Depth 100
}

try {
    $godot = Start-Process -FilePath $GodotExecutable `
        -ArgumentList @("--editor", "--path", $fixtureRoot) `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline -and -not $godot.HasExited) {
        $logs = ((Get-Content $stdoutPath, $stderrPath -ErrorAction SilentlyContinue) -join "`n")
        if ($logs -match "Named pipe server started" -and $logs -match "\[DidiSmoke\] scene opened") {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Assert-True $ready "Godot editor did not start Didi IPC and open the smoke scene within $StartupTimeoutSeconds seconds."

    $requests = @(
        (@{ jsonrpc = "2.0"; id = 1; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 2 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 3 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 4 "scene_set_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority"; value = 12 }),
        (Tool-Request 5 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 6 "editor_undo" @{}),
        (Tool-Request 7 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 8 "editor_redo" @{}),
        (Tool-Request 9 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 10 "scene_instantiate_node" @{ node_type = "Node"; parent_path = "/root/SmokeRoot"; name = "Spawned"; properties = @{ process_priority = 21 } }),
        (Tool-Request 11 "scene_get_property" @{ target_node = "/root/SmokeRoot/Spawned"; property_name = "process_priority" }),
        (Tool-Request 12 "scene_remove_node" @{ target_node = "/root/SmokeRoot/Spawned" }),
        (Tool-Request 13 "editor_undo" @{}),
        (Tool-Request 14 "scene_duplicate_node" @{ target_node = "/root/SmokeRoot/Spawned" }),
        (Tool-Request 15 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/SpawnedCopy"; new_parent_path = "/root/SmokeRoot/Container"; keep_global_transform = $true }),
        (Tool-Request 16 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 3 }),
        (Tool-Request 17 "editor_undo" @{}),
        (Tool-Request 18 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 3 }),
        (Tool-Request 19 "viewport_capture_frame" @{ camera_identifier = "active_editor_view" }),
        (Tool-Request 20 "signal_connect" @{ emitter_node = "/root/SmokeRoot"; signal_name = "tree_entered"; target_node = "/root/SmokeRoot/Subject"; target_method = "queue_free" }),
        (Tool-Request 21 "scene_get_property" @{ target_node = "/root/SmokeRoot/Missing"; property_name = "name" }),
        (Tool-Request 22 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "phase_one_typo" }),
        (Tool-Request 23 "scene_set_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority"; value = "wrong-type" }),
        (Tool-Request 24 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 25 "scene_instantiate_node" @{ node_type = "Node"; parent_path = "/root/SmokeRoot"; name = "InvalidSpawn"; properties = @{ phase_one_typo = 1 } }),
        (Tool-Request 26 "scene_remove_node" @{ target_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 27 "editor_undo" @{}),
        (Tool-Request 28 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 29 "scene_get_hierarchy" @{ root_path = "/root/SmokeRoot/Container"; max_depth = 1 }),
        (Tool-Request 30 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/Subject"; new_parent_path = "/root/SmokeRoot/Container"; keep_global_transform = $true }),
        (Tool-Request 31 "editor_undo" @{}),
        (Tool-Request 32 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 33 "scene_get_hierarchy" @{ root_path = "Container"; max_depth = 1 }),
        (Tool-Request 34 "scene_get_hierarchy" @{ root_path = ".."; max_depth = 0 }),
        (Tool-Request 35 "scene_get_hierarchy" @{ root_path = "../.."; max_depth = 0 }),
        (Tool-Request 36 "scene_instantiate_node" @{ node_type = "Resource"; parent_path = "/root"; name = "InvalidNonNode" }),
        (Tool-Request 37 "scene_remove_node" @{ target_node = "/root" }),
        (Tool-Request 38 "scene_duplicate_node" @{ target_node = "/root" }),
        (Tool-Request 39 "scene_reparent_node" @{ target_node = "/root"; new_parent_path = "/root/SmokeRoot/Container" }),
        (Tool-Request 40 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/SpawnedCopy"; new_parent_path = "/root/SmokeRoot/Container" }),
        (Tool-Request 41 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/Container"; new_parent_path = "/root/SmokeRoot/Container/SpawnedCopy" }),
        (Tool-Request 42 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/Subject"; new_parent_path = "/root/SmokeRoot/Subject" }),
        (Tool-Request 43 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 3 })
    )

    $rawResponses = $requests | & $didiExecutable
    $responses = @($rawResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json -Depth 100 })
    Assert-True ($LASTEXITCODE -eq 0) "Didi MCP process exited with $LASTEXITCODE."
    Assert-True ($responses.Count -eq $requests.Count) "Expected $($requests.Count) JSON-RPC responses, received $($responses.Count)."

    $byId = @{}
    foreach ($response in $responses) { $byId[[int]$response.id] = $response }

    $hierarchy = Tool-Payload $byId[2]
    Assert-True ($hierarchy.execution_mode -eq "live") "Hierarchy was not attributed to live execution."
    Assert-True ($hierarchy.scene_tree.path -eq "/root/SmokeRoot") "Hierarchy root path is not editor-independent: $($hierarchy.scene_tree.path)"
    Assert-True ($hierarchy.scene_tree.children[0].path -eq "/root/SmokeRoot/Subject") "Hierarchy child path is not editor-independent: $($hierarchy.scene_tree.children[0].path)"

    Assert-True ((Tool-Payload $byId[3]).value -eq 7) "Fixture property did not start at 7."
    Assert-True ((Tool-Payload $byId[4]).undo_redo_registered) "Property mutation did not report a real UndoRedo transaction."
    Assert-True ((Tool-Payload $byId[5]).value -eq 12) "Property mutation was not observable."
    Assert-True ((Tool-Payload $byId[7]).value -eq 7) "Undo did not restore the property."
    Assert-True ((Tool-Payload $byId[9]).value -eq 12) "Redo did not restore the changed property."

    $instantiated = Tool-Payload $byId[10]
    Assert-True ($instantiated.node_path -eq "/root/SmokeRoot/Spawned") "Instantiate returned a non-portable node path."
    Assert-True ((Tool-Payload $byId[11]).value -eq 21) "Instantiate ignored initial scalar properties."
    $duplicated = Tool-Payload $byId[14]
    Assert-True ($duplicated.duplicated_node -eq "/root/SmokeRoot/SpawnedCopy") "Duplicate did not return its deterministic logical path: $($duplicated.duplicated_node)"
    $reparentedHierarchy = Tool-Payload $byId[16]
    $container = @($reparentedHierarchy.scene_tree.children | Where-Object name -eq "Container")[0]
    Assert-True (@($container.children.name) -contains "SpawnedCopy") "Reparent did not move the duplicate under Container."
    $restoredHierarchy = Tool-Payload $byId[18]
    Assert-True (@($restoredHierarchy.scene_tree.children.name) -contains "SpawnedCopy") "Undo did not restore the duplicate's original parent."

    $capture = $byId[19].result
    Assert-True (-not $capture.isError) "Live viewport capture failed: $($capture.content[0].text)"
    $image = @($capture.content | Where-Object type -eq "image")
    $captureMetadataContent = @($capture.content | Where-Object type -eq "text")
    Assert-True ($captureMetadataContent.Count -eq 1) "Viewport capture did not return exactly one metadata payload."
    $captureMetadata = $captureMetadataContent[0].text | ConvertFrom-Json -Depth 20
    Assert-True ($captureMetadata.execution_mode -eq "live") "Viewport capture metadata did not identify live execution."
    Assert-True ($captureMetadata.is_live_frame -eq $true) "Viewport capture metadata did not identify a live frame."
    Assert-True ($captureMetadata.source -eq "godot_editor_viewport_texture") "Viewport capture source was not the editor viewport texture."
    Assert-True ($image.Count -eq 1) "Viewport capture did not return exactly one image."
    Assert-True ($image[0].mimeType -eq "image/png") "Viewport capture MIME type is not PNG."
    Assert-True ($image[0].data.StartsWith("iVBORw0K")) "Viewport capture does not contain a PNG signature."
    $pngBytes = [Convert]::FromBase64String($image[0].data)
    $pngWidth = [System.Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($pngBytes, 16))
    $pngHeight = [System.Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($pngBytes, 20))
    Assert-True ($captureMetadata.resolution.width -eq $pngWidth) "Viewport metadata width did not match PNG IHDR width."
    Assert-True ($captureMetadata.resolution.height -eq $pngHeight) "Viewport metadata height did not match PNG IHDR height."

    Assert-True $byId[20].result.isError "Unimplemented signal tool returned fake success."
    Assert-True ($byId[20].result.content[0].text -match "no trustworthy execution path") "Unimplemented tool error is not actionable."
    Assert-True $byId[21].result.isError "Missing node lookup returned fake success."
    Assert-True $byId[22].result.isError "Unknown property lookup returned fake null success."
    Assert-True $byId[23].result.isError "Incompatible scalar property write returned fake success."
    Assert-True ((Tool-Payload $byId[24]).value -eq 12) "Rejected property write changed the live node."
    Assert-True $byId[25].result.isError "Unknown initial property returned fake instantiate success."
    $restoredOrder = @((Tool-Payload $byId[28]).scene_tree.children.name)
    Assert-True ($restoredOrder[0] -eq "Subject") "Remove undo did not restore the node's original sibling index: $($restoredOrder -join ', ')"
    Assert-True ($restoredOrder -notcontains "InvalidSpawn") "Rejected instantiation leaked a node into the edited scene."
    $nestedHierarchy = Tool-Payload $byId[29]
    Assert-True ($nestedHierarchy.scene_tree.path -eq "/root/SmokeRoot/Container") "Nested hierarchy root lost its absolute logical path: $($nestedHierarchy.scene_tree.path)"
    $reparentUndoOrder = @((Tool-Payload $byId[32]).scene_tree.children.name)
    Assert-True ($reparentUndoOrder[0] -eq "Subject") "Reparent undo did not restore the original sibling index: $($reparentUndoOrder -join ', ')"
    $relativeHierarchy = Tool-Payload $byId[33]
    Assert-True ($relativeHierarchy.scene_tree.path -eq "/root/SmokeRoot/Container") "Relative hierarchy root was not canonicalized: $($relativeHierarchy.scene_tree.path)"
    Assert-True $byId[34].result.isError "A parent-relative hierarchy path escaped the edited scene."
    Assert-True $byId[35].result.isError "A multi-level parent-relative hierarchy path escaped the edited scene."
    Assert-True $byId[36].result.isError "A non-Node ClassDB object was accepted as a scene node."
    Assert-True $byId[37].result.isError "The edited scene root was accepted for removal."
    Assert-True $byId[38].result.isError "The edited scene root was accepted for duplication."
    Assert-True $byId[39].result.isError "The edited scene root was accepted for reparenting."
    Assert-True (-not $byId[40].result.isError) "Cycle-test setup could not place SpawnedCopy under Container."
    Assert-True $byId[41].result.isError "An ancestor node was accepted for reparenting beneath its descendant."
    Assert-True $byId[42].result.isError "A node was accepted as its own new parent."
    $cycleHierarchy = Tool-Payload $byId[43]
    $cycleContainer = @($cycleHierarchy.scene_tree.children | Where-Object name -eq "Container")[0]
    Assert-True (@($cycleContainer.children.name) -contains "SpawnedCopy") "Rejected cyclic reparenting changed the hierarchy."
    Assert-True (@($cycleHierarchy.scene_tree.children.name) -contains "Subject") "Rejected self-reparenting changed the hierarchy."

    $engineErrors = @(
        Get-Content $stderrPath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'UndoRedo history mismatch|Parameter "t" is null|\[ERROR' }
    )
    Assert-True ($engineErrors.Count -eq 0) "Godot reported bridge errors:`n$($engineErrors -join "`n")"

    Write-Output "Godot Phase 1 integration passed: live hierarchy, scalar properties, all node mutations with UndoRedo, viewport PNG, and honest errors."
}
finally {
    if ($null -ne $godot -and -not $godot.HasExited) {
        Stop-Process -Id $godot.Id
    }
}
