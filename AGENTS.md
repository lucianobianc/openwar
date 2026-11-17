# Campaign Simulator — Initial Implementation Instructions

Read `docs/DESIGN.md` and `docs/TERRITORY.md` thoroughly before making any changes.

These documents define the project's design, architecture, and technical requirements. Treat them as authoritative. Do not invent requirements or implement features beyond the current milestone.

## Project language and documentation

Use English for project materials, including code identifiers, comments, documentation, README files, user-facing text, and commit messages. Existing source documents in Portuguese may be used as references.

Document project decisions, requirements, and technical rationale in terms that stand on their own. Leave out private conversation details, chat excerpts, and personal context.

## Technology and coding standards

- **Language:** C++20 (ISO/IEC 14882:2020).
- **Compilers:** GCC and Clang.
- **Build system:** GNU Autotools (`./configure`, `make`, `make check`, `make install`, `make clean`).
- **Target platforms:** Linux, FreeBSD, and Windows.
- **Dependencies:** Keep external dependencies minimal. Prefer the C++ standard library.

### Code simplicity and readability

Write straightforward, conventional C++ that an intermediate programmer can understand, maintain, and modify.

- Prefer simple classes, structs, functions, and standard containers.
- Use modern C++20 features when they genuinely improve clarity, safety, or maintainability.
- Avoid unnecessary templates, metaprogramming, complex inheritance hierarchies, and elaborate design patterns.
- Do not introduce abstractions merely for extensibility or hypothetical future requirements.
- Prefer explicit, understandable control flow over clever or overly compact expressions.
- Use descriptive names and keep functions focused on a single responsibility.
- Apply RAII and standard resource-management practices.
- Avoid manual memory management when standard library facilities suffice.
- Document non-obvious algorithms, assumptions, and important design decisions.
- Do not overengineer.

**The goal is simple, readable, maintainable C++20 — not maximal use of modern language features.**

## Architecture

The campaign simulation must operate independently of graphics, user interfaces, and game engines.

Maintain a single authoritative world state. Separate simulation logic from presentation and input handling.

The architecture must eventually support strategic, operational, and tactical simulation at different update frequencies, without duplicating or contradicting world state.

For the current milestone, implement only the foundational systems. Do not create speculative infrastructure for future subsystems.

Simulation time must be independent of rendering frame rates and wall-clock execution speed.

Ensure reproducible behavior through configurable random seeds and deterministic update ordering.

## First milestone: simulation foundation

Implement the following:

1. **Simulation clock:** Deterministic progression of simulated time, independent of real time.
2. **World state:** A central representation of the simulated world.
3. **Faction identifiers:** Stable identifiers for WEST, EAST, and neutral ownership where applicable.
4. **Sector representation:** Sector identities, properties, and foundational territorial state according to `docs/TERRITORY.md`.
5. **Sector adjacency:** Explicit representation and validation of neighboring sectors.
6. **Randomness:** A configurable, reproducible random number generator.
7. **Command-line executable:** Initialize a small example world, advance the simulation, and display its state.
8. **Automated tests:** Verify clock behavior, sector topology, faction identifiers, world initialization, and deterministic execution.

Follow the specifications in `docs/TERRITORY.md`, but implement only the structures and behavior necessary for this milestone.

Do not implement logistics, combat, aircraft, strategic AI, graphics, or gameplay yet.

## Development process

Before implementation:

1. Review both design documents completely.
2. Inspect the existing repository and its build configuration.
3. Identify ambiguities that directly affect the first milestone.
4. Present a concise implementation plan.
5. Resolve relevant ambiguities before proceeding.

During implementation:

- Work incrementally.
- Keep changes limited to the agreed milestone.
- Add automated tests alongside the corresponding functionality.
- Avoid introducing unnecessary dependencies or architectural layers.
- Preserve existing code and repository conventions unless changes are justified.
- Keep public interfaces simple and document important invariants.

## Verification

Ensure that:

- The project builds with GCC and Clang using C++20.
- `make check` runs the automated tests.
- The simulation executes without graphical dependencies.
- Identical seeds and inputs produce identical results.
- Sector adjacency and world-state invariants are validated.
- The command-line executable initializes and advances a small campaign world correctly.

Report any verification that could not be performed.

## Delivery

At the end of the milestone:

1. Summarize the implementation.
2. Identify the main files and their responsibilities.
3. Report test results.
4. Explain any significant architectural decisions.
5. List remaining limitations and unresolved questions.

Do not begin the next milestone automatically.

**Priority: correctness, simplicity, determinism, and maintainability.**
