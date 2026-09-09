# On the Use of AI

> **This file was drafted by an AI.**
>
> That is the answer you came for, it is on the first line, and now we can spend the rest
> of the page on the parts that actually matter.

## The question

People ask whether AI helped build Didi. It comes up often enough that it is worth writing
down once, in one place, rather than answering it an issue at a time.

Yes. A great deal of it.

## Why the question lands a little differently here

Didi is a machine for handing a live Godot editor to a language model. That is not a side
effect of the design, it is the entire product: 115 canonical tools whose only purpose is
to let something that is not you move a node, patch a script, drive the game and look at
the result.

So asking whether an AI helped build it is a bit like asking a locksmith whether they own
a key.

This repository already contains a file written to be read by machines.
[`docs/LLM_INSTRUCTIONS.md`](docs/LLM_INSTRUCTIONS.md) is a system prompt and a decision
tree for the assistants that connect to this server, and it is addressed to them. This page
is addressed to you. It seemed only fair to have both.

## The recursion, stated in full

Let me stack it up honestly, because it is genuinely funny and pretending otherwise would
be a strange tone for a page about not pretending.

- A language model helped write a server that lets language models drive Godot.
- A language model is also its harshest tester. [`tools/field-trial/`](tools/field-trial/)
  hands an agent a Godot project, a briefing, and a build of Didi it has never seen, tells
  it to make an arena game using Didi for every action even when editing the file directly
  would obviously be faster, and then leaves. Nobody answers questions. It keeps a ledger
  of everything that went wrong and files what it can reproduce.
- Twenty-four issues in this repository carry the `field-trial` label. All twenty-four are
  closed.
- And a language model drafted this file, in which I disclose the use of language models.
  A document that is, structurally, the tool describing its own supply chain.

If somewhere in there you felt a snake start on its own tail, so did I. Keep reading and
make your own call.

## On whether a human wrote this

You are welcome to wonder, and prose style will not settle it for you. Here is a better
test: **look at what this document is willing to say.**

A model asked to write a friendly disclosure will write you a friendly disclosure. It will
not volunteer which three of its own tools do not work, name them, and link you to the
evidence. That section is two below this one. That is your tell.

The second tell is that the first draft had roughly twice as many jokes in it.

The third is that there is not a single em dash on this page. Not one. I took them all out
by hand, which is either proof that a human was here or precisely what a machine that has
read the same forum threads you have would do. I am aware this clears nobody. Nothing on
this page clears anybody, which is rather the point of the page.

## Who is responsible

Me.

Every line in here is a line I decided to ship. The distinction between code I typed and
code I read, ran, tested and accepted has never struck me as being as important as it
usually gets treated. If Didi drops a node in the wrong place, if a patch corrupts a script,
if undo eats your afternoon, that is not an interesting question about authorship. It is my
name on the repository.

## This project has a written policy against hallucinating

The characteristic failure of a language model is a confident, plausible answer where there
is no answer. Which makes it an odd thing to build a tool with, so it is worth knowing that
the hardest rule in this codebase exists specifically to forbid exactly that behaviour in
the software. From [`docs/ROADMAP.md`](docs/ROADMAP.md):

> **Do NOT add success stubs.** A registered name that cannot execute must report
> `implemented: false` and reject calls. This rule is absolute.

Three tool names are registered right now and refuse to run: `physics_simulate_step`,
`nav_bake_mesh` and `runtime_get_call_stack`. They were probed against pinned Godot 4.5.1 and
4.7.2 builds with the `extension_api.json` hashes recorded, and neither engine exposes a
public API that satisfies the approved contract. The reasoning, the pinned inputs and the
result hashes are in
[`docs/PHASE_7_API_FEASIBILITY.md`](docs/PHASE_7_API_FEASIBILITY.md). The count is 112 of
115, and the shortfall is printed on the box rather than hidden behind it.

Shipping three plausible-looking successes instead would have taken an afternoon. That
temptation is the thing this project is built to refuse, in the code and on this page.

## What actually checks it

"I test what it produces" is a cheap sentence. Here is what actually runs, all of it in this
repository, all of it runnable by you.

- **Over 800 automated tests**, more than 500 of them native C++, counted in
  [`docs/TEST_INVENTORY.md`](docs/TEST_INVENTORY.md), which is generated. The badge on the
  README derives from the suites themselves, and CI fails when the page and the suites
  disagree, so the number cannot quietly become a claim.
- **Live Godot integration in CI**, on real pinned engine builds across versions. Not mocks.
- **The documentation is validated against the build.** The smoke test compares the live
  `tools/list` surface to the manifest emitted by `didi --dump-tool-manifest` from that same
  build, and asserts every `implemented` flag rather than a sample. No tool count is ever
  typed into a workflow.
- `actionlint` and `zizmor` on the workflows, `ruff` on the Python, SHA-pinned actions,
  CodeQL, dependency review and an OpenSSF Scorecard.
- A protected `main` that takes no direct pushes from anyone, me included. Everything
  arrives as a pull request with green checks.

None of that is here because of AI. It is here because one person cannot hold 115 tools in
their head. But it is the reason I am willing to work this way: the checks do not care who
wrote a line, and they are not impressed by confidence.

## Where it is not much help

Specifics, because this is the part that makes the rest worth believing.

**It is confidently wrong about Godot.** The engine moves. A method exists on 4.7 and not on
4.5, or a bind changes shape between them, and the answer that arrives sounds exactly as
certain either way. Every API has to be checked against the `extension_api.json` for the
version actually in front of you.

**It forgets which thread it is on.** Editor mutation is main-thread work wrapped in an undo
transaction. Code that ignores that runs fine right up until it does not.

**It writes success stubs by instinct.** Left alone, a model will hand you a tool that
returns `{"ok": true}` and looks finished. Most of the rules quoted above exist because that
happened.

**Its favourite bug is reporting that the work was done.** From the field trial: a property
write that reported success and was silently discarded
([#213](https://github.com/saworbit/didi/issues/213)); a script patch that returned empty
diagnostics for a file it had just made unparseable
([#215](https://github.com/saworbit/didi/issues/215)); a resource written as JSON instead of
Godot literals, loading wrong, reporting success
([#327](https://github.com/saworbit/didi/issues/327)). One defect wearing three hats: the
report and the reality came apart. The fix was not to be more careful. It was the manifest
check and the schema contracts, which are not capable of being persuaded.

**It cannot tell you the idea is wrong.** It will help you build precisely the wrong thing,
quickly, with tidy commit messages. Deciding what not to build is still the whole job.

## The art

No image model was involved in anything that ships. The mark is geometry rather than a
render: every asset derives from the constants at the top of
[`docs/brand/build.py`](docs/brand/build.py), `raster.js` reproduces the committed PNGs
byte-for-byte from the SVG sources, and a test fails the build if the copies shipped inside
the addon drift from them. See [BRAND.md](docs/brand/BRAND.md).

If that ever changes, this is where it will say so first.

## Contributors

Yes, you may use it, on the same terms I do. The full version is in
[CONTRIBUTING.md](CONTRIBUTING.md), under AI-Assisted Contributions. The short version is
that nobody here is going to ask which lines you typed, and the checks will only ask whether
it works.

## If you would rather not

Some people want nothing to do with software built this way. That is a coherent position and
I am not going to argue anyone out of it.

I do think you are owed the information without having to excavate for it, which is why this
sits at the root of the repository. Read it, make your call, and go use something else with
no hard feelings.

My own view is narrow. A tool is worth using when it lets one person hold more of a hard
problem at once, and worth distrusting exactly to the degree that nothing is checking its
output. Didi is both halves of that sentence: the reaching into a live scene, and the
refusal to pretend it reached anywhere it did not. That is the whole design, and it is also,
as it happens, the entire content of this page.

## Provenance of this file

Drafted with an assistant, rewritten by hand, argued with, and cut. The em dashes went out
in the same pass as half the jokes. An AI wrote a document about AI, a human decided which
parts of it were true, and the byline is the human's, because that is where the liability
lives.

If you find that unsatisfying, consider the version of this file you would otherwise have
been handed: the same document, with this section missing.

Shane. Human, on the balance of evidence. Check the commit timestamps; nothing with a
scheduler works those hours.
