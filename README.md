<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Memory Passport

Memory Passport is an offline, three-button sequence memory tool for the
FoloToy AI Passport (ESP32-C3). The product shell opens with a splash and a
home screen for `Memory`, `Settings`, and `Exit`. Watch the `UP`, `DOWN`, and
`OK` cues, then repeat them in order. Each cleared round generates a fresh
random sequence one item longer, from level 3 to 12.

## MVP

- Visual and audio cues with a visual-only fallback.
- Five seconds for each expected input.
- Local best score stored in NVS; gameplay continues if storage is unavailable.
- Runtime brightness, sound, and slow/standard/fast pacing settings.
- Clock and battery status on the product shell, with elapsed-time fallback when no valid system clock exists.
- No account, network connection, leaderboard, or medical claim.

Controls: press `OK` to start or retry, use `UP` / `DOWN` / `OK` to answer,
and hold `OK` to return to the menu.

See the [product plan](docs/design/memory-training-product-plan.md),
[UI plan](docs/design/memory-training-ui-plan.md), and
[build instructions](docs/development/engineering/build-and-test.md).

This fork is based on [folotoy/ai-passport](https://github.com/folotoy/ai-passport).
