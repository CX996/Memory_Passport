<p align="right">
  <a href="memory-training-ui-plan.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Memory Passport UI Design Plan

> Document type: standalone UI design plan
> Product: FoloToy AI Passport
> Target: offline MVP
> Review status: implemented baseline

## 1. Design decision

Memory Passport is a focused product shell with a boot splash, a three-item home menu (`Memory`, `Settings`, `Exit`), and one sequence-memory activity. The user only needs to understand one loop: watch and hear an `UP / DOWN / OK` sequence, then repeat it in the same order. The initial sequence length is 3; every fully correct round generates a fresh random sequence one item longer, up to 12. A wrong item or a per-item timeout ends the session. The stored best is the highest sequence length fully completed locally, not an online rank and not a medical or cognitive assessment.

The first UI reuses the existing `ui_pixel` sky, grass, title plate, ink outline panels, and TV robot. On-device copy uses large ASCII labels, digits, arrows, and shapes. The current firmware enables Montserrat 14/20 but has no CJK glyphs; do not add a full Chinese font only for this page. Chinese prose belongs in this document and implementation comments, while the device uses short English labels.

## 2. Confirmed constraints and assumptions

| Item | Design constraint |
| --- | --- |
| Display | ST7789P3, 240 x 320 portrait RGB565. Use fixed-pixel layout and partial updates; do not depend on touch or MISO. |
| Memory | ESP32-C3 has no PSRAM. Do not create a full-screen double buffer or cache a long PCM stream. A static array of at most 12 three-value tokens is sufficient. |
| Input | The GPIO0 ADC resistor ladder yields `UP`, `DOWN`, and `OK`. Prefer logical `PRESS`; fall back to `CLICK` only when a driver variant does not deliver `PRESS`. |
| Global navigation | In the menu, `UP/DOWN CLICK` moves and `OK CLICK` enters. In the page, global routing intercepts `OK LONG` and returns to the menu. |
| Audio | ES8311 playback is blocking and must be consumed by an audio worker task. The game remains fully usable when audio is unavailable. |
| Battery | CW2017 is optional at runtime. When readable, show it in the clear sky below the top-right cloud; when the value is `-1`, hide it instead of drawing a fake number. |
| Persistence | Use the existing NVS partition; add no partition. Store only `best_level` (0-12) and the minimum schema/version data needed for migration. |
| Sequence | Generate a complete length-3 sequence at session start. After success, generate a complete new sequence at the next length; do not inherit the previous order or prefix. Tokens are independent, so adjacent repeats are valid. The random seed is not persisted. |
| Product shell | Show the boot splash once, then keep `Memory`, `Settings`, and `Exit` on the home screen. Give `Memory` the primary card and keep the two utility entries secondary. Settings expose brightness (25/50/75/100%), sound (on/off), and pace (slow/standard/fast); these values are runtime-only in this MVP. |
| Status | Show `HH:MM` when the system clock is valid. Without an RTC/NTP source, show elapsed device time from `00:00`; show the CW2017 percentage when readable and `--%` when unavailable. |
| Score terms | `level` is the sequence length currently being challenged; `clear_level` is the last fully completed length; `best_level = max(best_level, clear_level)`. |
| Audience | Default pacing targets ordinary adults while remaining readable, audible, and recoverable for older users. Make no medical claim and do not present the score as a health measure. |

## 3. Information architecture

```text
Boot splash
  └─ Home: MEMORY / SETTINGS / EXIT
      └─ MEMORY
      └─ READY
          └─ OK click -> PLAYBACK (device shows the sequence)
              └─ Playback complete -> INPUT (user repeats it)
                  ├─ All correct -> SUCCESS -> next-length PLAYBACK
                  ├─ Wrong item -> RESULT / WRONG
                  └─ Five seconds without input -> RESULT / TIME UP
              └─ OK long press (any feature state) -> main menu
          └─ Complete length 12 -> RESULT / MAX CLEAR
```

The MVP has no separate score page: `BEST` remains visible in READY and RESULT, avoiding an extra navigation step. Settings are intentionally limited to brightness, sound, and playback pace; there is no pause, answer replay, profile, or network flow.

## 4. Pages and states

The feature has one screen. A small state machine changes the same controls' copy, color, and accepted input. The state machine and timers should be pure logic; LVGL only renders the result.

| State | Screen focus | Entry/exit | Keys and timing |
| --- | --- | --- | --- |
| `READY` | `MEMORY`, `BEST n`, three-key legend, `OK START`, `HOLD OK BACK` | Entering the page, before a retry, or after a result | `OK CLICK` starts. `UP/DOWN CLICK` do not change settings and may give a light acknowledgement. `OK LONG` returns. No idle timeout. |
| `PLAYBACK` | `LEVEL n/12`, `WATCH`, highlighted token, `i/n` progress | Start from READY or automatically after SUCCESS | Ignore ordinary keys so the user cannot race the playback; `OK LONG` still exits. Highlight each token for about 360 ms with about 160 ms separation. |
| `INPUT` | `LEVEL n/12`, `YOUR TURN`, `i/n`, three large key cards, progress dots | Immediately after playback | Allow 5 seconds for each expected token. A correct key gives immediate feedback and advances; a wrong key ends the round. `OK LONG` exits; `DOUBLE` adds no input. |
| `SUCCESS` | `GOOD`, `CLEAR n`, one short green panel flash, robot jump | All expected tokens are correct | Lock ordinary input for about 900 ms. If `n < 12`, regenerate a complete random sequence at the next length and enter PLAYBACK; if `n = 12`, enter MAX RESULT. |
| `RESULT` | `WRONG` or `TIME UP`, `CLEAR n`, `BEST n`, `TRY AGAIN`, `HOLD OK BACK` | Wrong input, timeout, or max completion | `OK CLICK` starts a new session (show a 500 ms READY cue, then play length 3). `UP/DOWN CLICK` do not change the score. `OK LONG` returns. Keep the result on screen. |
| `MAX RESULT` | `MAX CLEAR`, `12/12`, `BEST 12` | Full completion of length 12 | Same controls as RESULT; never create length 13. |

### 4.1 Exact score semantics

- The first round is length 3. If it fails, show `CLEAR 0` and `FAILED AT 3`; do not count the unfinished length 3 as a best.
- If length 3 is completed and length 4 fails, show `CLEAR 3` and `FAILED AT 4`.
- Update the in-memory `clear_level` after each successful round. Request one NVS write only at session end when a new best exists.
- `BEST 12` means length 12 was fully completed and the MVP ceiling was reached. Do not show a percentage or an intelligence tier.

## 5. 240 x 320 layout sketch

Coordinates are a baseline, not a requirement to copy every pixel. Keep control sizes stable so the single DMA buffer can use local updates.

```text
 y=0   ┌──────────────────────── 240 ────────────────────────┐
       │  [MEMORY title plate]                    [battery]   │  8-44
       │                                                      │
 y=54  │  ┌──────────────────────────────────────────────┐  │
       │  │ LEVEL 4/12                 BEST 7             │  │  62-84
       │  │                 WATCH / YOUR TURN             │  │  88-116
       │  │   ┌──────┐       ┌──────┐       ┌──────┐       │  │
       │  │   │  UP  │       │ DOWN │       │  OK  │       │  │  124-182
       │  │   │  ↑   │       │  ↓   │       │  ●   │       │  │
       │  │   └──────┘       └──────┘       └──────┘       │  │
       │  │                 ● ● ○ ○                        │  │  190-208
       │  │                         HOLD OK BACK           │  │  214-226
       │  └──────────────────────────────────────────────┘  │
 y=238 │                    [TV robot]                       │
 y=286 │  grass footer (existing ui_pixel_screen_create)    │
 y=320 └──────────────────────────────────────────────────────┘
```

- Shell: call `ui_pixel_screen_create("MEMORY")`, retaining the fixed cloud (about `x=188,y=8`), grass from `y=286`, and title plate.
- Main panel: `x=12,y=54,w=216,h=178` is a useful starting point. Reuse `ui_pixel_panel_create()` with its 4 px ink edge, hard lower-right shadow, and inner padding. Avoid nested card stacks.
- Three key cards: equal widths of roughly 58-60 px, with a visual target of at least 56 x 44 px, around `x=18/88/158,y=124`. Each card shows both a word and a shape; color is never the only cue.
- Progress dots show how many inputs have been accepted, not the answer tokens, so they cannot leak the sequence. At most 12 dots can use two rows or 8 px squares.
- Reuse `ui_pixel_mascot_create()` around `y=238`. On success call the existing jump animation; on error keep the mascot still except for one result action.
- Put battery information in the open sky below the cloud (suggested `y=22-38`, or a 16 px icon if space is tight). Never cover the cloud or make battery data the primary training signal.

### 5.1 Content by state

| Region | READY | PLAYBACK | INPUT | RESULT |
| --- | --- | --- | --- | --- |
| Top | `BEST n` + clock/battery | `LEVEL n/12` + `WATCH` | `LEVEL n/12` + `YOUR TURN` | `CLEAR n` + `BEST n` |
| Center | Static three-key legend | Only the current token is highlighted | Pressed card briefly highlighted | Reason word (`WRONG`/`TIME UP`) |
| Bottom | `OK START` | `i/n` | `i/n` + progress dots | `TRY AGAIN` |
| Mascot | Blink | Small jump at playback start | Small jump for each accepted input | One result action only |

## 6. Three-key interaction contract

### 6.1 Event priority

1. `OK LONG` is global back and has priority over feature actions.
2. Training input consumes one logical press: prefer `PRESS`, or use `CLICK` in a driver variant without `PRESS`.
3. One physical action that yields `PRESS + CLICK` must count as one token. The time-window deduplication approach used by `origin/demo/tetris-game` is a suitable reference.
4. Ignore `DOUBLE` in the MVP; it must not become a hidden feature.
5. Lock ordinary input during playback and the success transition. Only an explicit `OK CLICK` on RESULT reopens a session.

### 6.2 Key table by state

| State | UP short | DOWN short | OK short | OK long |
| --- | --- | --- | --- | --- |
| READY | No state change; 80-120 ms light acknowledgement | Same | Start length 3 | Return to menu |
| PLAYBACK | Ignore | Ignore | Ignore | Abort and return |
| INPUT | Record `UP` | Record `DOWN` | Record `OK` | Exit without adding a token |
| SUCCESS | Ignore | Ignore | Ignore | Return to menu |
| RESULT/MAX | No state change | No state change | New session | Return to menu |

“No state change” is intentional: do not move a selection, change the score, or start the next round. A small mascot jump or a brief key-label highlight may confirm receipt, but do not play an action sound that could be mistaken for a game move.

### 6.3 Accidental input and long press

Resolve the `OK` long press in the input-reduction layer. If an `OK PRESS` arrives before the same action becomes `OK LONG`, it must not leave an irreversible `OK` token behind. If the current architecture can only forward `PRESS` immediately, use a short long-press suppression/rollback window in INPUT and verify on hardware that holding OK to leave never advances the sequence.

## 7. Visual hierarchy and color

| Meaning | Existing token | Use |
| --- | --- | --- |
| Page background | `UI_SKY` | Keep attention inside the main panel and preserve the brand sky. |
| Primary text/edge | `UI_INK` | Titles, numbers, borders, and shadows; high contrast on paper. |
| Normal panel | `UI_PAPER` | READY and INPUT surfaces. |
| Current highlight | `UI_YELLOW` plus white edge | Current playback token and press feedback; retain text and shape. |
| Success | `UI_GRASS` / `UI_GRASS_DARK` | `GOOD`, completion cue, and filled progress. |
| Warning | `UI_ORANGE` | Low battery and waiting hints, not failure. |
| Error | `UI_RED` | `WRONG` and `TIME UP` copy plus one border pulse. |

Keep this order: **state word > current level/progress > three-key action area > support data (`BEST`, battery)**. Decorations, the mascot, and battery numbers must not outrank `WATCH` or `YOUR TURN`.

The three key cards may use sky blue, orange, and yellow accents, but each must also contain `UP`/`DOWN`/`OK` text and `↑`/`↓`/`●` symbols. Do not communicate success only with green or failure only with red; pair both with a clear word and an audio/rhythm alternative.

## 8. Motion and sound

### 8.1 Motion timing baseline

| Event | Default treatment | Purpose |
| --- | --- | --- |
| Start | Hold READY for 500 ms, then highlight the first token | Give the user preparation time instead of an abrupt flash. |
| One playback token | Standard: highlight 360 ms, off/gap 160 ms; slow/fast are selectable in Settings | Separate neighboring cues without changing the generated sequence. |
| User key | Invert card/white edge for 180-220 ms and fill one progress dot | Make accepted input clear without sound. |
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

The audio worker consumes an event queue; button callbacks and LVGL callbacks only enqueue events. Do not force a long PCM write while NVS is committing. Save a new record on the result page or at an audio event boundary. If audio init/write fails, show a small `SOUND OFF` icon or label once on READY/RESULT and do not repeat the error every frame.

## 9. Errors, degradation, and recovery

| Failure | User-visible result | Boundary |
| --- | --- | --- |
| Display/LVGL init failure | No feature page | This is a system hard dependency; follow the existing main flow and stop rather than faking a text-only page. |
| Button init failure | Menu entry is marked `[FAIL]` and cannot be entered | Never create a second ADC unit inside the feature. |
| ES8311 unavailable or playback failure | Visual sequence continues; `SOUND OFF` | Do not change sequence rules or mark an answer wrong because sound is missing. |
| CW2017 unavailable | Hide the battery icon/percentage | Do not block training. If readable and below 20%, use an orange/red hint without forcing exit. |
| NVS init/read failure | `BEST --` or `SAVE OFF`; this session still shows its result | Do not erase unrelated NVS. The score may be lost on reboot. |
| NVS write failure | Brief `NOT SAVED` on RESULT | Do not roll back the completed score; log once and avoid per-frame retries. |
| Input queue full or duplicate event | No extra token is advanced | Drop duplicate/stale events and use bounded logging; let the user retry from RESULT. |
| Low battery | Top-right warning | Do not force sleep in the middle of a round; follow the existing power policy. |

Every failure message must include an understandable next action (continue watching, retry, or hold to leave). Do not make an error code the only explanation. Keep RESULT actionable instead of hiding failures with an automatic redirect.

## 10. Older-user and accessibility strategy

- **Never rely on color alone.** Every state has a large `WATCH`, `YOUR TURN`, `GOOD`, `WRONG`, or `TIME UP` word; every key has text and a shape.
- **Stable and predictable.** Keep the state word and key legend in the same locations. Avoid scrolling or sudden reflow. Show `OK START`, `TRY AGAIN`, and `HOLD OK BACK` explicitly.
- **Enough response time.** Use a 5-second per-item timeout by default. Test the standard 360/160 ms playback rhythm and the slow/fast settings on hardware; adjust timing constants only if needed and do not change the random-sequence rules.
- **Visual fallback for hearing limits.** When audio is off or the speaker fails, highlight duration, progress dots, and copy still provide the complete flow. Never remove critical feedback in a silent mode.
- **Audio fallback for vision limits.** Tones are cues, not the only code. Each token also has a distinct position, word, and shape.
- **Avoid risky flicker.** Success and error use at most one short pulse; never repeat flashes above 3 Hz. If animation fails, retain a static result.
- **Lower the cost of mistakes.** Do not use double click. UP/DOWN on RESULT never restarts. The long-OK back behavior is always the same.
- **Font and contrast.** Reuse Montserrat 14/20 with high-contrast `UI_INK`/`UI_PAPER`. If Chinese copy becomes mandatory, add only a glyph subset or bitmap font after checking internal RAM, largest free block, and refresh performance.
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
- `main/main.c` owns the boot splash, home menu, settings, status labels, and exit page.
- Since `ui_pixel_screen_create()` already supplies the brand structure, do not introduce a second theme or generic page factory.

## 12. MVP scope

### Must ship

- Boot splash, a home menu with `MEMORY`, `SETTINGS`, and `EXIT`, plus one memory screen covering READY, PLAYBACK, INPUT, SUCCESS, and RESULT/MAX.
- Three-value sequences starting at length 3; each success regenerates a complete next-length sequence, capped at 12. Adjacent tokens may repeat.
- Visual highlighting plus three distinguishable tones; complete visual fallback when audio fails.
- Runtime brightness, sound, and slow/standard/fast pace controls; clock and battery status remain visible on the shell.
- `PRESS`/`CLICK` deduplication, a 5-second per-item timeout, immediate failure on a wrong item, and global `OK LONG` back.
- `BEST` persisted in NVS. NVS failure must not block the current session and must expose `SAVE OFF`/`NOT SAVED`.
- Optional top-right battery display, low-battery hint, and the existing `ui_pixel` visual identity.
- Pure-model host tests for full-sequence regeneration, correct/wrong input, timeout, cap 12, score update, and storage boundaries.

### Explicitly out of scope

- Pause, profiles, daily goals, network sync, or leaderboards. Settings persistence and manual clock editing are also deferred.
- Answer replay, per-key review, voice input, touch, vibration, or new hardware interfaces.
- A full CJK font, complex pixel illustrations, continuous background music, or per-frame telemetry.
- Medical, educational-effect, or cognitive-ability claims.

## 13. Acceptance checklist

### Host logic

1. A length-3 sequence can be completed; each success regenerates a complete next-length sequence without inheriting the previous prefix; no sequence is generated after length 12.
2. A wrong token immediately enters RESULT; five seconds without input enters `TIME UP`; only a full completion updates `clear_level`.
3. `best_level` is the maximum fully completed length, including 0 and 12 boundaries. Missing, corrupt, and failed NVS paths all degrade visibly.
4. `PRESS + CLICK` from one physical action counts once; `DOUBLE` does not change the model; `OK LONG` leaves no token behind.

### Device

1. On the 240 x 320 portrait screen, splash, home cards, settings rows, title, cloud, clock, battery, panel, key cards, and dots do not overlap; partial updates do not tear.
2. Holding each key confirms the ADC windows and short/long boundaries; rapid repeated input neither loses nor duplicates positions.
3. In a quiet room and for users with hearing limits, the three tones are distinguishable; with audio disabled, the visual flow remains complete.
4. After completing a new best and rebooting, `BEST` remains; simulated NVS errors do not erase other data.
5. Low-battery and unavailable-gauge behavior is correct; inspect internal RAM, largest free block, and long-session stability.

## 14. Extension order

1. **Low-risk experience:** pause/resume, replay of the previous round, explicit clear-best action, and persisted settings; keep the three-key single-action model.
2. **Localization:** add a short CJK glyph subset or bitmap font only after RAM and refresh budgeting, then replace the ASCII labels.
3. **Companion play:** daily goals, streaks, and more visual themes; never package statistics as health conclusions.
4. **Connectivity:** BLE score sync or multiplayer with separate privacy, pairing, power, and offline-fallback design; do not make networking a prerequisite for the MVP.

The MVP succeeds when a user can complete one round without a manual and can tell why it ended and what the best score is. No extension may weaken that criterion.
