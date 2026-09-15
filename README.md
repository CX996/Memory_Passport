<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Memory Passport

Memory Passport is an offline, three-button sequence memory game for the
FoloToy AI Passport (ESP32-C3). Watch the `UP`, `DOWN`, and `OK` cues, then
repeat them in order. Each cleared round adds one cue, from level 3 to 12.

## MVP

- Visual and audio cues with a visual-only fallback.
- Five seconds for each expected input.
- Local best score stored in NVS; gameplay continues if storage is unavailable.
- No account, network connection, leaderboard, or medical claim.

Controls: press `OK` to start or retry, use `UP` / `DOWN` / `OK` to answer,
and hold `OK` to return to the menu.

See the [product plan](docs/design/memory-training-product-plan.md),
[UI plan](docs/design/memory-training-ui-plan.md), and
[build instructions](docs/development/engineering/build-and-test.md).

This fork is based on [folotoy/ai-passport](https://github.com/folotoy/ai-passport).
