# ESP-Mosaico ai_recorder agent rules

These rules apply to the application in this repository. The repository is the
application: the components live at the root, and there is no parent
workspace.

## Engineering rules

1. **Keep `main` a composition root.** `app_main()` does initialization,
   dependency wiring and startup sequencing, nothing else. Application
   components live under `components/app_*`; `main` composes them.

2. **Define contracts for public interfaces.** Keep the public API comment
   within three lines, describing the purpose and the key preconditions. The
   detailed ownership, lifecycle, calling context, thread safety, timeouts and
   error contracts belong in the component README, with units and ranges
   beside the fields.

3. **Keep application components apart from third-party ones.** Application
   components are `components/app_*`. Third-party components keep their
   upstream names and are managed centrally. Do not edit
   `managed_components/` in place; apply a change through a patch or an
   `override_path`.

4. **Separate the public interface from the implementation.** Public headers
   live in `include/`; internal headers are reachable only through private
   include paths. One component must not include another component's private
   headers.

5. **Encapsulate the state of a module.** Keep the state in a private context
   or a class. Exchanging the state of the application through cross-module
   `extern` globals is not allowed.

6. **Prefer composition and limit the abstractions.** Compose through members,
   contexts and small interfaces. An inheritance hierarchy or a generic
   framework that serves no practical purpose is a cost.

7. **Use named constants.** A domain value or a resource limit is a named
   constant, an enum, or a value in the shared configuration.

8. **Check the fallible steps.** Handle an allocation that fails, roll back a
   partial initialization, and give the mutable state and the resources an
   explicit owner and a lifetime. Synchronize the access across tasks, or
   document the single-owner design.

## Stack and allocation

- Avoid a local array or an aggregate object larger than 128 bytes on a task
  stack unless the task budget justifies it, including the call chain.
  Prefer a buffer the instance owns; do not move a mutable buffer into a
  hidden global.
- Prefer a reusable instance buffer, a memory pool or a ring buffer on a
  path that runs often or has a deadline. Do not allocate on the heap in
  those paths.

## Build and device work

- Build with the pinned ESP-IDF and the build workflow in
  `submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/`.
  `./bootstrap.sh` takes the pinned revision for you.
- Device work goes through `mosaico.py`. Do not call `idf.py flash`,
  `esptool` writes or ESP-Iris device-write commands directly, and do not
  guess a flash offset or bypass a compatibility check.
- Prove a change that alters behaviour on the device before the next
  behavioural change is stacked on it. Batch only the changes that cannot
  alter the behaviour.
- Accept an application only with live evidence: the identity of the device,
  its state, and the behavior the product is supposed to have.

## The dependencies

The three dependencies are submodules of this repository, pinned to a
reviewed revision:

| Submodule | Repository |
| --- | --- |
| `submodule/esp-mosaico-utils` | the product CLI, the Vibe Mode firmware, ESP-Iris |
| `submodule/esp-mosaico-bsp` | the board support package |
| `submodule/esp-rainmaker-neo-firmware` | the RainMaker Neo firmware |

A change that needs something from one of them goes into that repository, and
this repository moves the pin.

## Quality

The format rules, the project policy and the hook setup live in
`tools/quality/`. See its README to enable the hooks. A check that passes
does not establish an architecture or an API contract; those need a review.
