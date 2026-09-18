<p align="right">
  <a href="memory-training-ui-plan.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Memory Passport UI Design Plan

> Document type: standalone UI design plan
> Product: FoloToy AI Passport
> Target: offline MVP
> Review status: implemented baseline

## 1. Design decision

Memory Passport is a focused product shell with a boot splash, a standard game menu, and one sequence-memory activity. The menu presents five vertically centered, evenly spaced actions in this order: start game, choose difficulty, basic settings, more features, and exit. In the game, the user watches and hears an up/down/confirm sequence, then repeats it in the same order. The initial length follows the selected difficulty (2 relaxed, 3 standard, or 4 challenge; standard is the default). Each fully correct round generates a fresh random sequence one item longer without inheriting content from the prior round, up to 12. A wrong item or a per-item timeout ends the session. The stored best is the highest sequence length fully completed locally, not an online rank and not a medical or cognitive assessment.

The first UI reuses the existing `ui_pixel` sky, grass, full-width paper status bar, framed paper page panel, and TV robot. Every user-visible label on the boot and product screens is Simplified Chinese; internal state and event identifiers may remain English. A firmware port should bundle only the Chinese glyphs used by these screens rather than a complete general-purpose CJK font, then recheck internal RAM, largest free block, and refresh performance.

## 2. Confirmed constraints and assumptions

| Item | Design constraint |
| --- | --- |
| Display | ST7789P3, 240 x 320 portrait RGB565. Use fixed-pixel layout and partial updates; do not depend on touch or MISO. |
| Memory | ESP32-C3 has no PSRAM. Do not create a full-screen double buffer or cache a long PCM stream. A static array of at most 12 three-value tokens is sufficient. |
| Input | The GPIO0 ADC resistor ladder yields `UP`, `DOWN`, and `OK`. Prefer logical `PRESS`; fall back to `CLICK` only when a driver variant does not deliver `PRESS`. |
| Global navigation | In the menu, `UP/DOWN CLICK` moves across the five entries and `OK CLICK` enters. The displayed key labels are Simplified Chinese equivalents of up, down, and confirm. In a feature page, global routing intercepts `OK LONG` and returns to the menu. |
| Audio | ES8311 playback is blocking and must be consumed by an audio worker task. The game remains fully usable when audio is unavailable. |
| Battery | CW2017 is optional at runtime. Show its percentage when readable; when the value is `-1`, keep `--%` in the status bar instead of drawing a fake value. |
| Persistence | Use the existing NVS partition; add no partition. Store only `best_level` (0-12) and the minimum schema/version data needed for migration. |
| Difficulty | Provide relaxed, standard, and challenge levels. Their initial lengths are 2, 3, and 4; per-item input limits are 7, 5, and 4 seconds; cue timing is 580, 460, and 360 ms. Standard is the default. |
| Sequence | Generate a complete initial sequence from the selected difficulty. After every successful round, generate a complete new sequence at the next length; do not inherit the previous order or prefix. Tokens are independent, so adjacent repeats are valid. The random seed is not persisted. |
| Product shell | After the boot splash, show five vertically centered and evenly spaced entries in this order: start game, choose difficulty, basic settings, more features, and exit. Basic settings contain screen brightness and game sound only. More features contains instructions, best record, and version information; it has no hardware-test entry. |
| Status | Do not show or synthesize wall-clock time. Keep the left status-bar slot blank, center the page title, and show the CW2017 percentage at right when readable or `--%` when unavailable. |
| Score terms | `level` is the sequence length currently being challenged; `clear_level` is the last fully completed length; `best_level = max(best_level, clear_level)`. |
| Audience | Default pacing targets ordinary adults while remaining readable, audible, and recoverable for older users. Make no medical claim and do not present the score as a health measure. |

## 3. Information architecture

```text
Boot splash
  └─ Main menu: start game / choose difficulty / basic settings / more features / exit
      ├─ Start game -> READY
      │   └─ Confirm click -> PLAYBACK (device shows the sequence)
      │       └─ Playback complete -> INPUT (user repeats it)
      │           ├─ All correct -> SUCCESS -> regenerate next length -> PLAYBACK
      │           ├─ Wrong item -> RESULT (wrong)
      │           └─ No input before the selected limit -> RESULT (timeout)
      │       └─ Confirm long press (any game state) -> main menu
      │   └─ Complete length 12 -> MAX RESULT (all complete)
      ├─ Choose difficulty: relaxed / standard / challenge
      ├─ Basic settings: screen brightness / game sound
      ├─ More features: instructions / best record / version information
      └─ Exit: safe-power-off message
```

The best record remains visible in READY and RESULT and can also be opened from more features. Difficulty clearly shows and confirms relaxed, standard, or challenge. Basic settings contains only screen brightness and game sound. More features contains only instructions, best record, and version information; it has no hardware test, profile, or network flow.

## 4. Pages and states

The product-shell pages share Chinese copy, a battery-only status bar, a full-page framed paper panel, and three-key navigation:

| Page | Required content |
| --- | --- |
| Boot | Product mark and a short Chinese startup message, then automatic entry to the menu. |
| Main menu | Five vertically centered, evenly spaced Chinese entries for start game, choose difficulty, basic settings, more features, and exit. Up/down moves and confirm enters. |
| Difficulty | Chinese relaxed / standard / challenge choices plus the current selection and corresponding initial length, input limit, and cue timing. |
| Basic settings | Chinese screen-brightness and game-sound rows only. |
| More features | Chinese instructions, best record, and version-information entries only; no hardware test. |
| Exit | Chinese exit-confirmation or safe-power-off message with a three-key route back to the main menu. |

The feature has one screen. A small state machine changes the same controls' copy, color, and accepted input. The state machine and timers should be pure logic; LVGL only renders the result.

| State | Screen focus | Entry/exit | Keys and timing |
| --- | --- | --- | --- |
| `READY` | Chinese “Memory training”, “Best record n”, three-key legend, “Press confirm to start”, “Hold confirm to return” | Entering the page, before a retry, or after a result | `OK CLICK` starts at the selected difficulty. `UP/DOWN CLICK` do not change settings; the robot moves to and jumps on the matching key block. `OK LONG` returns. No idle timeout. |
| `PLAYBACK` | Chinese “Round n”, “Remember”, highlighted token, `i/n` progress | Start from READY or automatically after SUCCESS | Ignore ordinary keys so the user cannot race playback; `OK LONG` still exits. Use 580 / 460 / 360 ms cue timing for relaxed / standard / challenge. |
| `INPUT` | Chinese “Round n”, “Repeat”, `i/n`, three large key blocks, progress dots | Immediately after playback | Allow 7 / 5 / 4 seconds per expected token for relaxed / standard / challenge. A correct key gives immediate feedback and advances; a wrong key ends the round. `OK LONG` exits; `DOUBLE` adds no input. |
| `SUCCESS` | Chinese “Complete”, “Passed n”, one short green panel flash, robot jump | All expected tokens are correct | Lock ordinary input for about 900 ms. If `n < 12`, regenerate a complete random sequence at the next length and enter PLAYBACK; if `n = 12`, enter MAX RESULT. |
| `RESULT` | Chinese “Wrong” or “Timeout”, “Passed n”, “Best record n”, “Try again”, “Hold confirm to return” | Wrong input, timeout, or max completion | `OK CLICK` starts a new session (show a 500 ms ready cue, then play the selected difficulty's initial length). `UP/DOWN CLICK` do not change the score. `OK LONG` returns. Keep the result on screen. |
| `MAX RESULT` | Chinese “All complete”, `12/12`, “Best record 12” | Full completion of length 12 | Same controls as RESULT; never create length 13. |

### 4.1 Exact score semantics

- The first round length follows the selected difficulty (2 relaxed, 3 standard, 4 challenge). If it fails, show Chinese “Passed 0” and the corresponding “Failed at n”; do not count the unfinished first round as a best.
- For example, after completing length 3 on standard and failing at length 4, show Chinese “Passed 3” and “Failed at 4”.
- Update the in-memory `clear_level` after each successful round. Request one NVS write only at session end when a new best exists.
- “Best record 12” means length 12 was fully completed and the MVP ceiling was reached. Do not show a percentage or an intelligence tier.

## 5. 240 x 320 layout sketch

Coordinates are a baseline, not a requirement to copy every pixel. Keep control sizes stable so the single DMA buffer can use local updates.

```text
 y=0   ┌──────────────────────── 240 ────────────────────────┐
       │  [blank]             [Memory]             [battery] │  0-25
 y=32  │  ┌────── full-page framed paper panel ──────────┐  │
       │  │ Round 4                     Best record 7    │  │  56-80
       │  │                 Remember / Repeat             │  │  84-112
       │  │                    ● ● ○ ○                    │  │  120-144
       │  │                Hold confirm to return          │  │  154-174
       │  └──────────────────────────────────────────────┘  │
 y=194 │     [robot moves along the three key blocks]       │
 y=244 │    ┌──────┐       ┌──────┐       ┌──────┐          │
       │    │  up  │       │ down│       │confirm│          │  244-284
       │    └──────┘       └──────┘       └──────┘          │
 y=286 │  grass footer (existing ui_pixel_screen_create)    │
 y=320 └──────────────────────────────────────────────────────┘
```

- Shell: reuse the `ui_pixel_screen_create()` cloud and grass. Use a full-width paper status bar with a blank left slot, a strictly centered page title, and battery at right; do not render time.
- Page panel: reuse `ui_pixel_panel_create()` with its 4 px ink edge, hard lower-right shadow, and inner padding. Use `x=8,y=32,w=224,h=275` for shell pages and `x=8,y=32,w=224,h=204` for game pages, leaving room for the robot and key blocks. Avoid nested card stacks.
- Three key blocks: equal widths and spacing along the bottom, each showing the Chinese label for up, down, or confirm plus its shape. The robot uses one horizontal track above the blocks.
- Progress dots show how many inputs have been accepted, not the answer tokens, so they cannot leak the sequence. At most 12 dots can use two rows or 8 px squares.
- Reuse `ui_pixel_mascot_create()`. On every accepted short press, move it to the matching key block and play one jump; the animation must not alter input validation or replay for a deduplicated `PRESS + CLICK`.
- Keep only battery in the fixed status bar. The blank left slot balances the battery column so the title stays centered; never cover the cloud or make battery data the primary training signal.

### 5.1 Content by state

| Region | READY | PLAYBACK | INPUT | RESULT |
| --- | --- | --- | --- | --- |
| Panel top | Chinese “Best record n” | Chinese “Round n” + “Remember” | Chinese “Round n” + “Repeat” | Chinese “Passed n” + “Best record n” |
| Center | Static three-key legend | Only the current key block is highlighted | Pressed block briefly highlighted | Reason word (Chinese “Wrong”/“Timeout”) |
| Bottom | Chinese “Press confirm to start” | `i/n` | `i/n` + progress dots | Chinese “Try again” |
| Mascot | Moves to an accepted key block and jumps | Ordinary input locked | Moves to each accepted key block and jumps | One result action only |

## 6. Three-key interaction contract

### 6.1 Event priority

1. `OK LONG` is global back and has priority over feature actions.
2. Training input consumes one logical press: prefer `PRESS`, or use `CLICK` in a driver variant without `PRESS`.
3. One physical action that yields `PRESS + CLICK` must count as one token. The time-window deduplication approach used by `origin/demo/tetris-game` is a suitable reference.
4. Ignore `DOUBLE` in the MVP; it must not become a hidden feature.
5. Lock ordinary input during playback and the success transition. Only an explicit `OK CLICK` on RESULT reopens a session.

### 6.2 Key table by state

| State | Up short | Down short | Confirm short | Confirm long |
| --- | --- | --- | --- | --- |
| READY | No state change; robot moves to and jumps on the up block | Same for down | Robot confirms, then starts the selected difficulty | Return to menu |
| PLAYBACK | Ignore | Ignore | Ignore | Abort and return |
| INPUT | Record up; robot moves and jumps | Record down; robot moves and jumps | Record confirm; robot moves and jumps | Exit without adding a token |
| SUCCESS | Ignore | Ignore | Ignore | Return to menu |
| RESULT/MAX | No state change; robot moves and jumps | Same | Robot confirms, then starts a new session | Return to menu |

“No state change” is intentional: do not move a selection, change the score, or start the next round. Each received short press triggers exactly one robot move-to-block and jump-back animation; locked, duplicate, or long-press-upgraded events trigger no confirmation animation.

### 6.3 Accidental input and long press

Resolve the `OK` long press in the input-reduction layer. If an `OK PRESS` arrives before the same action becomes `OK LONG`, it must not leave an irreversible `OK` token behind. If the current architecture can only forward `PRESS` immediately, use a short long-press suppression/rollback window in INPUT and verify on hardware that holding OK to leave never advances the sequence.

## 7. Visual hierarchy and color

| Meaning | Existing token | Use |
| --- | --- | --- |
| Page background | `UI_SKY` | Keep attention inside the main panel and preserve the brand sky. |
| Primary text/edge | `UI_INK` | Titles, numbers, borders, and shadows; high contrast on paper. |
| Normal panel | `UI_PAPER` | READY and INPUT surfaces. |
| Current highlight | `UI_YELLOW` plus white edge | Current playback token and press feedback; retain text and shape. |
| Success | `UI_GRASS` / `UI_GRASS_DARK` | Completion copy, cue, and filled progress. |
| Warning | `UI_ORANGE` | Low battery and waiting hints, not failure. |
| Error | `UI_RED` | Wrong and timeout copy plus one border pulse. |

Keep this order: **state word > current level/progress > three-key action area > support data (best record, battery)**. Decorations, the mascot, and battery numbers must not outrank the Chinese “Remember” or “Repeat” prompt.

The three key blocks may use sky blue, orange, and yellow accents, but each must also contain the Chinese up/down/confirm label and `↑`/`↓`/`●` symbols. Do not communicate success only with green or failure only with red; pair both with clear Chinese copy and an audio/rhythm alternative.

## 8. Motion and sound

### 8.1 Motion timing baseline

| Event | Default treatment | Purpose |
| --- | --- | --- |
| Start | Hold READY for 500 ms, then highlight the first token | Give the user preparation time instead of an abrupt flash. |
| One playback token | Relaxed / standard / challenge cue timing: 580 / 460 / 360 ms | Difficulty changes pacing without changing the independent random generation rule. |
| User key | Invert the matching block/white edge; move the robot to it and jump once; fill one progress dot | Make the physical key and visual feedback correspond one-to-one. |
| Success | One green panel flash of about 180 ms plus the existing 110/140 ms jump; total hold about 900 ms | Reward clearly without glare. |
| Error/timeout | One red-edge pulse of about 220 ms, then a still result page | Avoid rapid flicker; state the reason in words. |

Animations must degrade to static color and copy when frame rate or partial refresh is poor. Do not require continuous full-screen redraws. Reuse the mascot's existing blink timing and do not add several infinite animations.

### 8.2 Tone proposal

Audio is a secondary channel; visual highlighting is authoritative. Start with short mid-frequency square-wave or simple PCM notes, then calibrate frequency and volume on hardware:

| Token/event | Suggested pitch | Duration |
| --- | ---: | ---: |
| `UP` | about 523 Hz | 180-220 ms |
| `DOWN` | about 659 Hz | 180-220 ms |
| `OK` | about 784 Hz | 180-220 ms |
| `GOOD` | 523 -> 659 -> 784 Hz | 90-120 ms each |
| `WRONG` | about 220 Hz low note | 220-300 ms |
| `TIME UP` | two descending notes | 300-400 ms total |
| `MAX CLEAR` | rising three-note phrase plus long note | no more than 700 ms total |

The audio worker consumes an event queue; button callbacks and LVGL callbacks only enqueue events. Do not force a long PCM write while NVS is committing. Save a new record on the result page or at an audio event boundary. If audio init/write fails, show a small Chinese “Sound unavailable” message once on READY/RESULT and do not repeat the error every frame.

## 9. Errors, degradation, and recovery

| Failure | User-visible result | Boundary |
| --- | --- | --- |
| Display/LVGL init failure | No feature page | This is a system hard dependency; follow the existing main flow and stop rather than faking a text-only page. |
| Button init failure | Menu entry shows a Chinese “Buttons unavailable” message and cannot be entered | Never create a second ADC unit inside the feature. |
| ES8311 unavailable or playback failure | Visual sequence continues; Chinese “Sound unavailable” | Do not change sequence rules or mark an answer wrong because sound is missing. |
| CW2017 unavailable | Keep `--%` in the status bar | Do not block training. If readable and below 20%, use an orange/red hint without forcing exit. |
| NVS init/read failure | Chinese “Best record --” or “Cannot save”; this session still shows its result | Do not erase unrelated NVS. The score may be lost on reboot. |
| NVS write failure | Brief Chinese “Not saved” on RESULT | Do not roll back the completed score; log once and avoid per-frame retries. |
| Input queue full or duplicate event | No extra token is advanced | Drop duplicate/stale events and use bounded logging; let the user retry from RESULT. |
| Low battery | Top-right warning | Do not force sleep in the middle of a round; follow the existing power policy. |

Every failure message must include an understandable next action (continue watching, retry, or hold to leave). Do not make an error code the only explanation. Keep RESULT actionable instead of hiding failures with an automatic redirect.

## 10. Older-user and accessibility strategy

- **Never rely on color alone.** Every state has a large Chinese “Remember”, “Repeat”, “Complete”, “Wrong”, or “Timeout” word; every key has Chinese text and a shape.
- **Stable and predictable.** Keep the state word and key legend in the same locations. Avoid scrolling or sudden reflow. Show Chinese “Press confirm to start”, “Try again”, and “Hold confirm to return” explicitly.
- **Enough response time.** Use 7 / 5 / 4 seconds per item for relaxed / standard / challenge. Test the 580 / 460 / 360 ms playback rhythm on hardware; adjust only these timing constants and do not change the independent random-sequence rule.
- **Visual fallback for hearing limits.** When audio is off or the speaker fails, highlight duration, progress dots, and copy still provide the complete flow. Never remove critical feedback in a silent mode.
- **Audio fallback for vision limits.** Tones are cues, not the only code. Each token also has a distinct position, word, and shape.
- **Avoid risky flicker.** Success and error use at most one short pulse; never repeat flashes above 3 Hz. If animation fails, retain a static result.
- **Lower the cost of mistakes.** Do not use double click. UP/DOWN on RESULT never restarts. The long-OK back behavior is always the same.
- **Font and contrast.** Use a high-contrast `UI_INK`/`UI_PAPER` palette and only the Chinese glyph subset used by the screens; recheck internal RAM, largest free block, and refresh performance.
- **Health boundary.** Use terms such as training, round, and highest completed length. Do not claim memory improvement, diagnosis, or treatment.

## 11. NVS and implementation handoff

### 11.1 Minimal data model

```text
namespace: memory
key:       best_level   uint8 (0..12)
optional:  schema       uint8
```

Treat a missing key as `best_level=0`. Request one write only in RESULT/MAX RESULT when `clear_level > best_level`; never write Flash during playback of each token, each key, or each LVGL tick. The asynchronous-save pattern from the Pomodoro branch is useful, but a single byte does not justify full session restoration.

### 11.2 Boundary with the current project

- Keep the page, state machine, timers, animations, and sequence model in `main`; do not extend `components/bsp` for this feature.
- Button callbacks only enqueue into the existing input queue. A non-LVGL task must hold `bsp_lvgl_lock()` before changing controls.
- Put audio and NVS work in worker tasks. Stop timers/tasks before deleting the page screen and clear object pointers.
- Expected implementation pieces are `main/demo_memory.c` and a small `memory_model` independent of LVGL/ESP-IDF, registered through `demo.h`, `main/CMakeLists.txt`, and `main.c`.
- `main/main.c` owns the boot splash, five-entry menu, difficulty page, basic settings, more features, status labels, and exit page.
- Since `ui_pixel_screen_create()` and `ui_pixel_panel_create()` already supply the status bar and framed page structure, do not introduce a second theme or generic page factory.

## 12. MVP scope

### Must ship

- A fully Chinese boot splash; a five-entry home menu ordered as start game, choose difficulty, basic settings, more features, and exit; and one memory screen covering READY, PLAYBACK, INPUT, SUCCESS, and RESULT/MAX.
- Relaxed / standard / challenge start at lengths 2 / 3 / 4, use 7 / 5 / 4 second input limits, and use 580 / 460 / 360 ms cue timing.
- Every completed round regenerates a complete next-length random sequence, capped at 12, without inheriting prior content. Adjacent tokens may repeat.
- Visual highlighting plus three distinguishable tones; complete visual fallback when audio fails.
- Basic settings contains only screen brightness and game sound. More features contains only instructions, best record, and version information. The shell keeps its centered title and right-aligned battery, with no time display.
- The bottom robot moves to the matching physical up/down/confirm block and jumps for each accepted short press. Deduplicate `PRESS`/`CLICK`, apply the selected difficulty's timeout, fail immediately on a wrong item, and retain global `OK LONG` back.
- The best record is persisted in NVS. NVS failure must not block the current session and must expose Chinese “Cannot save” or “Not saved”.
- Keep the battery position in the status bar, show `--%` when unavailable, and retain the low-battery hint and existing `ui_pixel` visual identity.
- Pure-model host tests for full-sequence regeneration, correct/wrong input, timeout, cap 12, score update, and storage boundaries.

### Explicitly out of scope

- Pause, profiles, daily goals, network sync, or leaderboards. Settings persistence and any wall-clock display or editing are also out of scope.
- Answer replay, per-key review, voice input, touch, vibration, or new hardware interfaces.
- A general-purpose font covering all CJK, complex pixel illustrations, continuous background music, or per-frame telemetry.
- Medical, educational-effect, or cognitive-ability claims.

## 13. Acceptance checklist

### Host logic

1. The three difficulties start at lengths 2 / 3 / 4; each success regenerates a complete next-length sequence without inheriting the previous prefix; no sequence is generated after length 12.
2. A wrong token immediately enters RESULT; exceeding the selected 7 / 5 / 4 second input limit enters the timeout result; only a full completion updates `clear_level`.
3. `best_level` is the maximum fully completed length, including 0 and 12 boundaries. Missing, corrupt, and failed NVS paths all degrade visibly.
4. `PRESS + CLICK` from one physical action counts once; `DOUBLE` does not change the model; `OK LONG` leaves no token behind.

### Device

1. On the 240 x 320 portrait screen, the blank-left / centered-title / battery-right status bar and the full-page framed paper panel match the approved layout; the Chinese splash, five-entry menu, difficulty page, basic settings, more features, exit page, key blocks, and dots do not overlap; partial updates do not tear.
2. Holding each key confirms the ADC windows and short/long boundaries. Each accepted short press moves the robot to the matching block and jumps exactly once; rapid input neither loses nor duplicates positions, counts, or animations.
3. In a quiet room and for users with hearing limits, the three tones are distinguishable; with audio disabled, the visual flow remains complete.
4. After completing a new best and rebooting, the best record remains; simulated NVS errors do not erase other data.
5. Low battery produces a warning and an unavailable gauge leaves `--%` in the status bar; inspect internal RAM, largest free block, and long-session stability.

## 14. Extension order

1. **Low-risk experience:** pause/resume, replay of the previous round, explicit clear-best action, and persisted settings; keep the three-key single-action model.
2. **Additional languages:** Chinese is the first-release baseline. Reuse the layout for later languages and recheck glyph, RAM, and refresh budgets.
3. **Companion play:** daily goals, streaks, and more visual themes; never package statistics as health conclusions.
4. **Connectivity:** BLE score sync or multiplayer with separate privacy, pairing, power, and offline-fallback design; do not make networking a prerequisite for the MVP.

The MVP succeeds when a user can complete one round without a manual and can tell why it ended and what the best score is. No extension may weaken that criterion.
