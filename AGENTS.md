# Working in this repository

Pointers for anyone, person or coding agent, asked to pick up work on Didi.

- **What to work on next.** Open `P0` and `P1` issues first, as
  [ISSUE_SEVERITY.md](ISSUE_SEVERITY.md) describes. After that, the next
  capability is the first row in [docs/BUILD_QUEUE.md](docs/BUILD_QUEUE.md) whose
  status is `PLANNED` and whose dependencies are all `COMPLETE`. "Build the next
  capability" means that row.
- **Why the surface is shaped this way.**
  [docs/DESIGN_PRINCIPLES.md](docs/DESIGN_PRINCIPLES.md). Each queue item names
  the principles it has to keep, and the refusals there are not to be built.
- **Adding a tool name.** An entry in
  [docs/SURFACE_AMENDMENTS.md](docs/SURFACE_AMENDMENTS.md) comes first.
- **Building, testing and the gates a pull request must pass.**
  [CONTRIBUTING.md](CONTRIBUTING.md).
- **Finishing a queue item.** The pull request that meets the item's
  **Done when** sets its row to `COMPLETE (#<pull request>)`, closes its issue,
  and updates the phase status in [docs/ROADMAP.md](docs/ROADMAP.md) if the
  phase moved.
