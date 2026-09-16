<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 记忆通行证

“记忆通行证”是运行在 FoloToy AI Passport（ESP32-C3）上的离线三键记忆训练工具。
设备开机先显示启动页，然后进入包含 `Memory`、`Settings`、`Exit` 的主界面。设备依次提示
`UP`、`DOWN`、`OK`，用户按相同顺序复现；每通过一轮就重新生成一串长度增加一项的随机序列，
从长度 3 逐步训练到 12。

## 首版功能

- 视觉与音调双重提示，声音不可用时仍可完成训练。
- 每个待输入项有 5 秒作答时间。
- 最高完整复现长度保存在本机 NVS；存储不可用时本局仍可继续。
- 支持运行时亮度、声音和慢/标准/快节奏设置。
- 主界面显示时间和电量；没有有效系统时钟时使用设备运行时间。
- 不需要账号或联网，不提供排行榜，也不作医疗效果宣称。

操作方式：短按 `OK` 开始或重试，用 `UP` / `DOWN` / `OK` 作答，长按 `OK`
返回菜单。

详细资料见[功能设计方案](docs/design/memory-training-product-plan.zh_CN.md)、
[界面设计方案](docs/design/memory-training-ui-plan.zh_CN.md)和
[构建说明](docs/development/engineering/build-and-test.zh_CN.md)。

本项目基于 [folotoy/ai-passport](https://github.com/folotoy/ai-passport) 开发。
