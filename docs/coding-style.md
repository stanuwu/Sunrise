# Coding style

How code in this build is written, and why it reads the way it does. This is
descriptive, not aspirational: every rule here is already followed by the files
around you. When a new file disagrees with an old one, the old one wins — match
what is there.

The formatting is enforced by `.clang-format` (LLVM base, 100 columns, 4-space
indent, attached braces). Run it before every commit; format-on-save handles it
in Visual Studio. Everything below is what clang-format *cannot* enforce — the
conventions a reviewer will otherwise ask you to fix by hand.

## Names

| Kind | Case | Example |
| --- | --- | --- |
| Functions | `snake_case` | `resolve_native_equipment_slot`, `ensure_character_emote_collection` |
| Types (struct/enum/class) | `PascalCase` | `ProfileItem`, `EmoteCollectionOutcome` |
| Constants | `kPascalCase` | `kEmoteCollectionSlot`, `kHiddenRigOffset` |
| Variables, members | `camelCase` | `nativeSlot`, `characterIndex`, `definitionHash` |
| Namespaces | `snake_case` | `sunrise::state::account::inventory` |

Literals carry their type: `3183180185U` (unsigned), `1'000'000.0F` (float),
with digit separators on anything long enough to miscount. A bare `50` in a
comparison is a smell — see the next section.

## No magic numbers, ever

Every constant is named, `constexpr`, and carries a doc comment saying what it
is and where it came from. This is the single most visible rule in the codebase.

```cpp
/**
 * Definition hash of the real, non-equippable "Emotes" collection item. The Client opens its own
 * wheel-configuration screen for this exact item once it is equipped with valid socket data.
 */
inline constexpr std::uint32_t kEmoteCollectionDefinitionHash = 3183180185U;
```

A hardcoded `3183180185U` sitting in an `if` would not survive review. If the
number came from reading the game's data, the comment says so; if it was found
empirically, the comment says *that* ("confirmed empirically in-game").

## Doc comments on everything

Every function, struct, enum, and constant gets a `/** ... */` block. Functions
use `@param` / `@return`. The comment explains **why**, not what — the code
already says what.

```cpp
/**
 * Resolves the native equipment slot a configured item detail occupies.
 * Every character-scoped item declares its own native slot except the "Emotes" collection item
 * (kEmoteCollectionDefinitionHash), the one item whose real content has none. Any other item
 * missing a native slot is rejected instead of silently aliasing this fallback.
 * @param definitionHash Authored item definition hash being resolved.
 * @param detailEquipmentSlot The installed item detail's own native slot, if it declares one.
 * @param nativeSlot Receives the resolved native slot on success.
 * @return True when the item declares its own non-negative slot, or is the Emotes collection item.
 */
[[nodiscard]] bool resolve_native_equipment_slot(...) noexcept;
```

Inline comments carry the same weight. When a line exists because of a
non-obvious constraint, the comment names the constraint:

```cpp
// The verbosity setter logs through this same funnel; without this it would recurse.
```

The best comments in this codebase describe the *failure that would happen
without this line*. Aim for those.

## `noexcept` and `[[nodiscard]]`

- Almost everything is `noexcept`. This code runs inside a live game process; a
  thrown exception is not an option. There is no `throw`, and hot paths use no
  STL containers that allocate — fixed `std::array` buffers instead.
- Every function that returns a status (`bool`, an outcome enum) is
  `[[nodiscard]]`. A result that can be ignored is a bug waiting to happen.

## Rich outcomes, not bare bool

A function that can fail in distinct ways returns an enum that names each way,
so the caller can tell "nothing to do" from "could not be done." This is
everywhere, and it is deliberate:

```cpp
/** Why one attempt to canonicalize the "Emotes" collection item ended. */
enum class EmoteCollectionOutcome : std::uint8_t {
    /** Every character carries a sound collection item, either already or as of this call. */
    ready,
    /** The build data or account this reads is not published yet, so a retry is still owed. */
    notReady,
    /** The installed content does not carry the item this expects, so it can never be applied. */
    unsupported,
    /** The item could not be placed, so no character was changed and a retry is still owed. */
    failed,
};
```

Note each *enumerator* gets its own doc comment. A caller records the account as
canonical only on `ready` — collapsing `unsupported` and `failed` into one
`false` would lose the distinction the whole function exists to make.

## File and namespace structure

- One concern per file. A feature is a folder, not a big file — `client/hooks/
  <feature>/` with a `*_lifecycle.cpp/h` pair and the observer body beside it.
- Internal helpers live in an anonymous namespace at the top of the `.cpp`;
  only the public surface is declared in the `.h`.
- Namespaces mirror the path: a file under `state/account/inventory/` lives in
  `namespace sunrise::state::account::inventory`.
- Includes are grouped and sorted (clang-format's `IncludeBlocks: Regroup`);
  `<Windows.h>` always sorts first.

## Concurrency

Shared state is guarded by an `SRWLOCK`, acquired and released explicitly, with
a comment on any non-obvious ordering. Locks are held for the shortest span that
is still correct, and the reason a lock spans what it spans is written down:

```cpp
// A cache write holds its own lock across file calls, so a held thread stopped inside one
// would deadlock the freeze below.
AcquireSRWLockExclusive(&g_refreshLock);
```

## Logging

One structured line per event, `ev=key value` form, written with `snprintf`
into a fixed `std::array<char, N>`. Every skip or failure logs *why* — silence
is treated as a bug, because a skipped migration and a completed one must never
look the same in the log:

```cpp
"ev=queuez stage=account_preflight result=skip reason=%s"
```

## Idempotency

Anything that mutates account or build state is written to be safe to call more
than once: an item already correct is left alone, a corrupted one is repaired in
place keeping its identity, and the function says which happened. If you cannot
make an operation idempotent, say so in the doc comment and explain why.

---

## Documentation style (for `Sunrise/docs/`)

When a change required real reverse-engineering, the *findings* get their own
doc under `Sunrise/docs/`, written to the same standard as
[`emote-unlocks.md`](emote-unlocks.md). The shape that file established:

1. **Open with what, how, and why** — one paragraph, no preamble. State plainly
   that the data is not public and was read out of the packages directly if so.
2. **"Where X actually lives"** — the concrete offsets, slots, table shapes. Be
   specific enough that someone could verify you.
3. **Flag the traps.** The most valuable lines in `emote-unlocks.md` are the
   honest warnings — *"This is the part that silently wastes a day,"* *"Writing
   slot numbers as indices does nothing visible."* If something cost you a day,
   write it down so it costs the next person zero.
4. **Give the data**, in tables and code blocks, exactly.
5. **"Regenerating this"** — the steps to rebuild build-specific data when a
   content update invalidates it. Reproducibility over trust.
6. **"Known gap"** — end by naming what you did *not* capture. Honesty about the
   edges is part of the doc, not an admission of failure.

The voice is precise, unhedged, and free of filler. No marketing, no "simply,"
no hand-waving. If you are unsure whether a detail belongs, it belongs.

---

*Fastest way to internalize all of this: open one recently merged PR and read
the diff top to bottom. The style is more legible in a real change than in any
guide — including this one.*
