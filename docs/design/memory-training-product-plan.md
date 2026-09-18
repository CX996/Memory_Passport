<p align="right">
  <a href="memory-training-product-plan.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Memory Passport Functional Design

> Document type: Product functional baseline
> Target platform: FoloToy AI Passport (ESP32-C3, 240 x 320, three buttons, ES8311)
> Target release: Offline MVP
> Review status: implementation baseline

## 1. Product positioning

Memory Passport is a short, offline sequence-memory training tool. The device plays a sequence of
button cues, and the user reproduces the same sequence with the three physical buttons. Each fully
correct round unlocks a fresh random sequence one item longer, until the user makes a mistake or
reaches the limit.

The boot screen and every product page use Simplified Chinese. After boot, a standard game menu shows
five entries in this order: start game, choose difficulty, basic settings, more features, and exit. The
layout targets the 240 x 320 portrait display and three physical buttons.

The product offers a small, repeatable cognitive-training activity. It is not a medical assessment.
Scores describe performance on this device only and must not be presented as evidence of improved
memory, health status, or cognitive ability.

The default audience is general adults, with readable and audible behavior suitable for older users.
Children's and older-user variants can tune pace and copy on the same model instead of splitting the
first release into separate products.

## 2. Goals and non-goals

### 2.1 MVP goals

- A user can start a round within one minute without a manual.
- A round normally lasts 30 seconds to 3 minutes and can be restarted immediately after failure.
- The activity works through visual and audio cues; it remains fully playable when audio is disabled
  or unavailable.
- The highest fully reproduced length survives a reboot.
- The gameplay model can be verified by host tests without ESP-IDF or hardware.
- The product shell has a fully Chinese boot splash, a five-entry game menu, difficulty page, basic
  settings, more features, and exit page. Its full-width status bar leaves the left slot blank, keeps
  the page title centered, and shows battery at right; content sits in the framed paper page panel.

### 2.2 Out of scope for the first release

- Local or cloud models, accounts, networking, leaderboards, and cross-device sync.
- Medical diagnosis, ability ratings, or claims about training effects.
- Complex daily streaks, calendar statistics, or social systems. The device has no reliable
  absolute-time source, so the shell does not display or synthesize wall-clock time.
- Large image sets, GIFs, long recordings, or continuous background music.
- Touch, vibration, NFC interaction, or new board-level interfaces.
- A developer-facing hardware-test entry in the product menu; hardware diagnostics are not a primary feature.

## 3. Core user flow

```text
Boot screen
  -> Main menu: start game / choose difficulty / basic settings / more features / exit
       |- Start game -> ready -> device plays the sequence -> user reproduces it
       |    |- Complete and correct -> success feedback -> generate a fresh longer sequence -> play again
       |    `- Wrong or timeout -> result screen -> retry or return
       |- Choose difficulty -> relaxed / standard / challenge
       |- Basic settings -> screen brightness / game sound
       |- More features -> instructions / best record / version information
       `- Exit -> safe-power-off message
```

The user needs to understand only two actions: watch/listen to the cue, then reproduce it. No text
entry, complex setup, or phone connection is required during a round.

## 4. MVP rules

### 4.1 Sequence

- Three tokens map to `UP`, `DOWN`, and `OK`.
- Relaxed / standard / challenge start at lengths 2 / 3 / 4, allow 7 / 5 / 4 seconds per input,
  and use 580 / 460 / 360 ms cue timing. Standard is the default.
- Every session generates a fresh initial sequence at the selected difficulty's starting length.
- After the current sequence is reproduced correctly, generate a complete new random sequence at
  the next length. Do not inherit the previous round's order or prefix.
- The first-release maximum is length 12. A complete length-12 round goes to the result state; no
  length-13 sequence is generated.
- Use a session-local pseudo-random generator. Each token is independent; adjacent tokens may repeat
  because that is a valid random result.
- The seed is session-local and is not stored in NVS, so a reboot may produce a different sequence.

### 4.2 Device cues

- Highlight the matching button and play a short, distinct tone for each token.
- Relaxed / standard / challenge use 580 / 460 / 360 ms per cue. Tune only these difficulty values on
  real hardware without changing the independent random-generation rule.
- Ignore ordinary input while the device is playing, so a user cannot race the playback. `OK` long
  press still exits.
- Enter the input state immediately after playback and show the Chinese equivalent of “Repeat”.

### 4.3 User reproduction

- Accept one normalized logical press at a time, in exact sequence order.
- Prefer the `PRESS` event. Fall back to `CLICK` only when the installed button component cannot
  reliably provide `PRESS`.
- The same physical action can produce both `PRESS` and `CLICK`; deduplicate it so it counts once.
- Give each expected token the selected difficulty's 7 / 5 / 4 second limit. A timeout is a failed
  round and uses a Chinese timeout label.
- End the round immediately on a wrong token and use a Chinese wrong-answer label. Do not automatically replay the
  entire answer, so the error position remains meaningful.
- Give immediate visual progress feedback for a correct token. Do not write Flash for each input.
- For every accepted short press, move the bottom robot to the matching up/down/confirm block and make
  it jump once. This gives a one-to-one mapping between physical input and visual feedback; duplicate
  or locked events do not animate.

### 4.4 Button semantics

| State | `UP` short | `DOWN` short | `OK` short | `OK` long |
| --- | --- | --- | --- | --- |
| Ready | Move and jump on the up block; no setting change | Move and jump on the down block; no setting change | Move and jump on confirm, then start at the selected difficulty | Return to main menu |
| Playback | Ignore | Ignore | Ignore | Abort and return |
| Input | Enter `UP`; move and jump | Enter `DOWN`; move and jump | Enter `OK`; move and jump | Abort and return |
| Success transition | Ignore | Ignore | Ignore | Return to main menu |
| Result | Move and jump; no score change | Same | Confirm visually, then start a new session | Return to main menu |

Ignore `DOUBLE` events in the MVP. Keep the repository-wide long-`OK` return behavior. An incomplete
sequence does not count toward the best score and is not saved as a resumable half-session.

## 5. State model

Keep gameplay logic independent from LVGL, audio drivers, and NVS. At minimum, model these states:

| State | Meaning | Transition |
| --- | --- | --- |
| `READY` | Waiting to start; show selected difficulty and best record | `OK CLICK` -> generate a fresh initial sequence for the selected difficulty -> `PLAYBACK` |
| `PLAYBACK` | Play the current sequence item by item | End -> `INPUT`; long `OK` -> main menu |
| `INPUT` | Wait for the user's reproduction | All correct -> `SUCCESS`; wrong/timeout -> `RESULT` |
| `SUCCESS` | Short feedback for a completed round | Length < 12 -> `PLAYBACK`; length 12 -> `MAX_RESULT` |
| `RESULT` | Failed-round summary | `OK CLICK` -> new session; long `OK` -> main menu |
| `MAX_RESULT` | Summary after reaching the first-release limit | `OK CLICK` -> new session; long `OK` -> main menu |

The model accepts normalized inputs and time events such as `start`, `token`, `tick`, and `long_back`.
It does not call `lv_*`, `bsp_audio_*`, or `nvs_*` directly.

## 6. Score definitions

Use separate names for the current challenge and the completed result:

- `challenge_level`: length currently being challenged.
- `clear_level`: last length fully reproduced in this session; 0 when the first round fails.
- `best_level`: largest length ever fully reproduced, in the range 0..12.
- `session_count`: number of started sessions, optional statistic.
- `correct_inputs` / `total_inputs`: optional cumulative statistics, not emphasized in the MVP UI.

Examples:

- Fail on the first round: show the Chinese equivalent of “Passed 0”; the best record is unchanged.
  The first length follows the selected difficulty.
- Clear length 3 on standard, then fail at length 4: show the Chinese equivalent of “Passed 3”;
  update the best record only if it was below 3.
- Clear length 12: show the Chinese equivalents of “Passed 12” and “Best record 12”, then enter `MAX_RESULT`.

The primary MVP metrics are only the Chinese best-record and session-passed values. Do not display an
intelligence level, percentile, or comparison with other users.

## 7. Persistence and recovery

### 7.1 Minimal data

Use the existing NVS partition and a namespace such as `memory`:

```text
schema_version  uint8   Data format version
best_level      uint8   0..12
```

The first release persists only `best_level`. Difficulty, screen brightness, and game sound are runtime
settings with safe defaults after reboot; persist them only when a product requirement justifies the
extra NVS schema and migration surface. If sound is disabled or unavailable, the visual flow remains complete.

### 7.2 Write timing

- Request one write in the result state only when a new record is produced.
- Never write Flash for each token, LVGL tick, or audio fragment.
- Treat a missing key as `best_level = 0`.
- On NVS read/write failure, continue the session and show the Chinese equivalent of “Cannot save” or “Not saved”; never erase the
  partition automatically.
- On an incompatible version, use safe defaults and log the condition. Add migration or dual-slot CRC
  only when the data model grows enough to justify it.

### 7.3 Power-loss semantics

The MVP does not resume an incomplete session. After a reboot, return to `READY` and retain the prior
`best_level`; an interrupted session earns no score. This keeps recovery simple and avoids storing a
full sequence and timer state.

## 8. Audio and visual fallback

- Give each token a distinct short tone; use separate short cues for success, wrong input, and timeout.
- Prefer generating small PCM blocks at runtime. Do not embed large audio assets or allocate a long
  recording buffer.
- Run audio playback in a worker task. Button callbacks only enqueue events and return quickly.
- If ES8311 is unavailable, playback fails, or audio is disabled, visual highlights and text still
  provide the complete flow.
- Audio is never the only authoritative cue: labels, positions, symbols, and progress indicators
  must stand on their own.

## 9. Accessibility and safety boundary

- Do not use color alone to identify a token. Show Chinese up/down/confirm labels plus symbols.
- Use stable, high-contrast Chinese status labels for remember, repeat, complete, wrong, timeout, and retry.
- Give each difficulty a clear input limit and cue timing. Parameter changes do not alter the
  independent random-sequence rule.
- Use at most one short success/error pulse; avoid high-frequency flashing or glaring continuous motion.
- Do not claim treatment, memory improvement, or diagnosis in UI copy.
- Keep training data local. Do not collect names, voice, location, or network identifiers.

## 10. Implementation handoff

The MVP needs no `components/bsp` change, new dependency, or new partition:

| File | Responsibility |
| --- | --- |
| `main/memory_model.c/.h` | Pure sequence, state, input validation, and score logic |
| `main/demo_memory.c` | Page lifecycle, input forwarding, and LVGL presentation |
| `main/memory_store.c/.h` | NVS read/write and graceful failure (can start inside the demo) |
| `main/main.c` | Fully Chinese boot splash; five-entry menu; difficulty page; screen-brightness/game-sound settings; instructions/best-record/version pages; centered-title/battery-only status bar; framed page panel; and exit page |
| `tests/test_memory_model.c` | Hardware-independent model tests |

Register the page through `main/demo.h`, `main/CMakeLists.txt`, and the `DEMOS[]` table in `main.c`.
Reuse `ui_pixel_screen_create()`, `ui_pixel_panel_create()`, and the existing mascot. Keep the approved
full-width status bar and full-page framed paper panel; do not create another theme or page factory.

Lifecycle rules remain mandatory:

1. Button callbacks only enqueue events; they do not access LVGL, play audio, or write NVS.
2. Stop audio and timer tasks with a bounded handshake before leaving the page.
3. Non-LVGL tasks take `bsp_lvgl_lock()` before touching objects.
4. Stop every timer/task/callback, then delete the screen and clear object pointers.

## 11. Automated acceptance

### 11.1 Host logic

- The three difficulties start at lengths 2 / 3 / 4, and each success regenerates a complete sequence
  at the next length without inheriting the previous prefix.
- Length 12 is the upper bound; length 13 is never generated.
- The 7 / 5 / 4 second difficulty limits and correct, wrong, timeout, empty, and out-of-range inputs
  produce deterministic states.
- Repeated adjacent tokens are accepted as valid random output.
- `PRESS + CLICK` counts once, `DOUBLE` changes nothing, and `OK LONG` leaves no token behind.
- `best_level` is updated only by a fully cleared length, with correct 0 and 12 boundaries.
- Duplicate completion events do not duplicate the score or write request.
- NVS defaults, incompatible versions, and write failures have isolated tests.

### 11.2 Device

- The fully Chinese boot screen, five-entry menu, difficulty, basic-settings, more-features and exit
  pages, status words, button legend, battery, and mascot do not overlap on the 240 x 320 display. The
  status bar has a blank left slot, centered title, and right-aligned battery, above the framed page panel.
- Short/long press boundaries are stable. Every accepted short press moves the robot to the matching
  block and jumps exactly once; rapid input loses or duplicates neither counts nor animations.
- The three tones are distinguishable in a quiet room; disabling or removing audio still permits a full
  round.
- Repeated page entry/exit does not continuously leak tasks, timers, LVGL objects, or heap.
- After a new record and reboot, the best record remains; NVS failure does not stop training.
- Record internal RAM, largest free block, and the audio task stack high-water mark.

## 12. Iteration order

1. **P0:** One sequence-memory mode, difficulty starts at 2 / 3 / 4 through the cap of 12,
   visual/audio cues, wrong/timeout results, and a persistent best record.
2. **P1:** On-device difficulty calibration, pause/resume, previous-round review, clear-score action,
   and richer feedback.
3. **P2:** Reverse memory, rhythm memory, visual-only mode, daily challenge, and BLE sync.
4. **P3:** Phone/desktop configuration or AI-generated training packs; keep the device usable offline.

Validate each stage before adding new states or resources. Do not introduce networking, NFC, large
images, long audio, and account systems together with P0.

## 13. Definition of done for the first release

The MVP is complete only when:

- A user can complete the full `start -> watch -> reproduce -> success/failure -> retry` loop alone.
- Rules, score definitions, and timeout behavior match this document and have host logic tests.
- The core activity remains usable with audio, battery, or NVS unavailable, with a clear fallback message.
- Display, buttons, audio, reboot persistence, and repeated page entry/exit are verified on real hardware.
- Build, host-test, and device-test results are reported separately; a successful build is not hardware
  validation.
