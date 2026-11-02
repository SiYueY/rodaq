# RoDAQ Agent Guide

This file is the entry point for coding agents working in this repository.
It exists to keep ChatGPT, Codex/DeepSeek, and other development agents aligned on the same repository-owned context.

Do not turn this file into a project encyclopedia. Detailed design belongs in `docs/design/`; current execution context belongs in `docs/development/`.

## 1. Mandatory startup sequence

Before modifying code, build files, tests, or design documents:

1. Read `docs/development/status.md`.
2. Read `docs/development/plan.md`.
3. Read the relevant entries in `docs/development/decisions.md`.
4. Read `docs/design/rodaq_v1_system_design.md`.
5. Read the detailed design document(s) for the component being changed.
6. Read `docs/coding_guidelines/cpp_coding_guidelines.md` before writing C++.

For Plugin System work, also read:

- `docs/design/rodaq_plugin_system.md`
- `docs/design/acquisition_plugin.md`
- `docs/design/storage_plugin.md`
- the corresponding backend document under `docs/design/acquisition_plugin/` or `docs/design/storage_plugin/`

For Runtime / data-path work, also read:

- `docs/design/architecture.md`
- `docs/design/canonical_data_model.md`
- `docs/design/canonical_profile.md`
- `docs/design/runtime.md`
- `docs/design/admission.md`
- `docs/design/payload.md`

## 2. Source-of-truth hierarchy

Use this precedence when information overlaps:

1. Explicit current user instruction.
2. `docs/design/rodaq_v1_system_design.md` for system-wide invariants.
3. The most specific `docs/design/*.md` specification for component-level contracts.
4. `docs/coding_guidelines/cpp_coding_guidelines.md` for C++ implementation rules.
5. Accepted entries in `docs/development/decisions.md`.
6. `docs/development/plan.md` and `status.md` for execution state.

If code conflicts with a design contract, do not silently make the code the new source of truth.
Report the conflict and either fix the implementation or deliberately revise the design before continuing.

## 3. Current V1 scope

Design scope:

- Acquisition Plugins: ROS 2 and DDS.
- Storage Plugins: MCAP and SQLite3.

First functional implementation:

- ROS 2 Acquisition.
- MCAP Storage.

DDS Acquisition and SQLite3 Storage follow after the common Runtime and first vertical slice are validated.

SocketCAN and other backends are future extensions and are not part of the current V1 freeze gate.

## 4. Non-negotiable architecture rules

- C++17; GCC 11.x is the reference compiler.
- Runtime is the only system-level mediator between Acquisition and Storage.
- Canonical Model is the semantic authority; backend-specific types must not leak into Runtime public contracts.
- One Canonical Stream has at most one active acquisition path at a time in V1.
- Control Plane state mutation is logically serialized.
- Acquisition may submit concurrently from multiple callback threads.
- Storage V1 uses one logical Runtime-to-Storage dispatch lane.
- Per Stream: AcceptanceSequence order equals accepted dispatch / Storage handoff order.
- All critical resources are explicitly bounded.
- Admission is correctness logic; a queue is transport, not the source of Accepted semantics.
- Before Gate Pass, reservation is all-or-nothing and rollback must be complete and non-failing.
- After Gate Pass, ordinary resource pressure must not silently drop an Accepted Record.
- Payload is one contiguous immutable logical byte range in V1.
- Payload ownership modes are Copy / Adopt / Retain.
- Acquisition submit keeps `AdmissionOutcome`, `RejectionReason`, and `PayloadDisposition` orthogonal. For Adopt/Retain, responsibility changes only after the defined transfer/acquisition point; rejection reason never implies ownership.
- Externally retained memory counts against Runtime payload budgets.
- Stop and failure paths must deterministically converge to quiescence.
- Plugin topology is frozen while Running; V1 dynamic loading does not mean hot reload.
- Plugin binary boundary is stable C ABI; the C++17 Plugin SDK is an authoring facade only.
- Acquisition and Storage use family-specific protocols, not one generic Plugin vtable.
- Plugin -> Runtime extensibility uses the Service Registry.
- Runtime -> Plugin optional capabilities use Extension Query.
- Storage submit semantics use independent Payload and Persistence axes as defined in `rodaq_plugin_system.md`: HandoffAccepted is established by successful `submit()`, Payload is `Pending/Consumed`, Persistence is `Pending/Succeeded/Failed/Unknown`, and each pending axis has its own exactly-once terminal notification.
- A DSO cannot be unloaded while any instance, callback, retained/adopted Payload release path, pending operation, or extension view can still execute Plugin code.

## 5. Engineering rules

- Prefer mature open-source infrastructure for generic mechanisms.
- RoDAQ owns domain semantics and correctness contracts.
- Do not introduce a dependency before checking the existing dependency baseline and whether the capability belongs in Core.
- Third-party production dependencies live under `third_party/` as pinned Git submodules; normal builds must not fetch from the network.
- Do not expose third-party implementation types in Public API or Plugin ABI.
- Keep implementation in `src/`; public/installable contracts belong in `include/rodaq/`.
- Avoid implementation-heavy headers unless templates or compile-time requirements make them necessary.
- Optimize only after a representative benchmark or profile identifies a bottleneck.
- Do not weaken correctness, boundedness, shutdown, or ABI contracts for a local performance optimization.
- This project is pre-release: do not preserve obsolete implementation compatibility unless a frozen V1 contract explicitly requires it.

## 6. Work protocol

Before starting a task:

- Confirm the active milestone and immediate task in `docs/development/plan.md`.
- Check `git status`; do not overwrite unrelated local work.
- Read only the design documents relevant to the task, plus the system baseline.

During implementation:

- Keep changes scoped to the active task.
- Add tests with the implementation where practical.
- If a frozen contract appears impossible or contradictory, stop changing that contract in code and record the issue for design review.
- Do not create extra design documents when an existing canonical document can be updated.

After a meaningful task:

1. Update `docs/development/status.md` with factual repository state.
2. Update `docs/development/plan.md` if milestone/task state changed.
3. Add to `docs/development/decisions.md` only when a durable architectural or engineering decision was made.
4. Run the relevant build/tests/static checks.
5. Review `git diff` for accidental scope expansion.
6. Do not mark work complete unless tests/validation actually ran, or explicitly state what was not run.

## 7. Shared-context discipline

The repository, not any model's chat history, is the durable project memory.

Store only material context:

- architecture decisions,
- accepted constraints,
- milestone state,
- blockers,
- validation results,
- next actionable work.

Do not store:

- full chat transcripts,
- transient debugging logs,
- ordinary fixed typos,
- speculative ideas that were not accepted,
- model-specific commentary.

`status.md` answers "Where are we now?"
`plan.md` answers "What are we doing next?"
`decisions.md` answers "What durable choices must future agents not rediscover?"
`docs/design/` answers "What is the system contract and why?"
