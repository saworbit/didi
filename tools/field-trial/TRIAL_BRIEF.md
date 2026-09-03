# Field Trial Briefing

Read all of this before you start.

## Your situation

- Your working directory is the Godot project you are building in. Everything you make goes there.
- An MCP server named `didi` is connected to this session. It is the tool you are here to use.
- Didi's source repository is at `D:\didi`. Read anything in it you like. Do not write to it, and do not run any git command inside it.
- Godot 4.7.2 is at `C:\Godot\Godot_v4.7.2-stable_win64.exe`, and `C:\Godot\Godot_v4.7.2-stable_win64_console.exe` when you need to capture output.
- You have never used Didi before. Work out what it can do.

## This run is unattended

Nobody will answer questions, so do not ask any. When you are blocked, write it down, pick the most reasonable interpretation, and keep going. Do not stop early because something is unclear.

## What to build

A single-screen 2D arena survival game.

1. The player is a `CharacterBody2D` moving in four directions through project input actions. Do not hardcode keycodes.
2. The arena is a TileMapLayer with solid walls the player cannot cross.
3. Three enemies come from a packed scene and chase the player. Each has an AnimationPlayer with at least one animation that plays.
4. Score lives on an autoload singleton and reaches a HUD Control label by signal. Do not poll for it.
5. Clearing all three enemies wins. A third hit on the player loses. Show a screen for each.
6. It runs. Launch the game, drive the player with injected input, capture a frame of the editor viewport and one of the running game, and show the logs are clean.

Get as far as you can. Partial is fine. Stopping without recording why is not.

## Use Didi first

Attempt every action through Didi first, including the ones where editing a file directly would obviously be faster. That is the point of the run.

When a tool fails or cannot do what you need, you may fall back to writing files directly or driving Godot yourself. Every fallback goes in the ledger.

## The ledger

Keep `TRIAL_LOG.md` in your working directory. Append an entry every time something does not go smoothly:

    ## [timestamp] Short title
    Intent:    what I was trying to achieve
    Attempt:   tool name and exact arguments
    Result:    exact response or error
    Verdict:   worked | worked-with-friction | failed
    Fallback:  what I did instead, or none
    Issue:     issue number, or none and why

Also record architectural decisions and node paths on Didi's blackboard, as its own documentation recommends.

## Filing issues

Issues go to the `saworbit/didi` repository with `gh`. Filing is deliberately expensive. Before you open one:

1. Re-read the relevant part of `D:\didi\docs`. Behaviour that is documented and wrong is still worth reporting, under a different label.
2. Search open and closed issues for a duplicate.
3. Reduce it to a minimal reproduction: exact tool name, exact arguments, exact response.
4. Choose a label. `bug` when behaviour contradicts the documentation. `documentation` when the documentation is wrong or missing. `enhancement` when the capability is simply absent.

Add the `field-trial` label to every issue on top of that classification.

One issue per root cause, never one per occurrence. Stop after twenty. Later findings go in the ledger with a note that the cap was reached.

Fill in the fields the bug report template asks for: Didi version, Godot version, operating system, reproduction, expected, actual.

Write in first person and in plain sentences. No em dashes, no emoji. Do not describe the report as generated, and do not name a model, an assistant, an agent, or a tool as its author.

Issues only. Do not commit, branch, push, or open a pull request anywhere.

## When you finish

Add a summary to the end of `TRIAL_LOG.md`: features completed, features abandoned and why, issues filed, and the one change to Didi that would have helped you most.
