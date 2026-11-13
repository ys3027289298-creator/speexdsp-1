# 通话中运行时采样率切换（48k → 44.1k → 32k）

## 结论

同一条通话流、同一个 `SpeexResamplerState` 上，在帧间调用
`speex_resampler_set_rate()` 即可完成 48 kHz → 44.1 kHz → 32 kHz 切换：

- 不中断流：不销毁/重建状态、不 flush、不插零，帧继续按 10 ms 喂入；
- 无可听突变：切换点一阶差分仅为稳态 p99.9 差分的 0.019 / 0.044 倍
  （对稳态中位数则为 0.068 / 0.162 倍，见 `verify_report.txt`）；
- 状态迁移：滤波器记忆 `mem`、分数相位 `samp_frac_num`、`last_sample`、
  `magic_samples` 由 `update_filter()`/`set_rate_frac()` 迁移，指针不变
  （测试中有 `st == state_handle` 断言）；
- 非法目标率：0 Hz 返回 `RESAMPLER_ERR_INVALID_ARG(3)`，空状态返回
  `RESAMPLER_ERR_BAD_STATE(2)`，失败后原速率与流状态保持不变；
- 单次切换耗时上界：4000 次实测最大 0.09–0.15 ms（p99.9 约 0.08–0.11 ms，
  中位约 0.019 ms），远小于 10 ms 帧长。

## 代码改动（小改，共三处）

- `libspeexdsp/resample.c`
  - `speex_resampler_set_rate_frac()`：空指针返回 `BAD_STATE`；切换失败时
    回滚速率/比例/相位并恢复旧滤波器，保证进行中的流仍可继续处理；
  - `speex_resampler_set_rate()`/`set_quality()`/`get_rate()`：空指针防护；
  - `speex_resampler_strerror()`：补齐 `RESAMPLER_ERR_OVERFLOW` 文案。
- `include/speex/speex_resampler.h`：补充运行时切换与失败语义注释。
- `libspeexdsp/testresample3.c`（新增仓库自带入口，已挂入
  `libspeexdsp/Makefile.am`）：模拟 180 帧通话，第 60/120 帧后切换，
    校验错误码、测量耗时、统计切换点连续性。

## 复现步骤（仓库自带入口）

```bash
./autogen.sh
./configure --enable-examples
make -j8

# C 入口：通话切换 + 错误码 + 耗时 + 连续性，产物写入 output/
./libtool --mode=execute libspeexdsp/testresample3

# Python 校验：离线一致性 / 连续性 / 耗时上界 / 错误码（仅标准库 + ctypes）
python3 output/verify_switch.py

# 分段录屏（标题 + 段间停顿，全自动，不等待键盘）
bash output/record_demo.sh
```

定点构建同样通过：`./configure --enable-fixed-point --enable-examples && make`。

## 验收证据

- `output/verify_report.txt`：自动化校验汇总，全部 PASS；
- `output/testresample3_stats.txt`：C 入口的切换耗时与连续性统计；
- `output/verify_live.f32` / `output/verify_reference.f32`：实时切换流与
  离线重放流，长度相同且逐采样完全相等（exact match PASS）；
- 稳态一致性：切换稳定后与“始终按 32 kHz 离线重采样”的结果在群延迟对齐
  （11 个采样）后，18689 个采样 `max|diff| = 0`；
- `output/source_48k.wav`：自造的小素材（1.8 s，440 Hz + 1 kHz 双音）；
- `output/segment1_48k.wav`、`output/segment2_44p1k.wav`、
  `output/segment3_32k.wav`：按各自真实采样率切分渲染的三段音频；
- `output/casts/segment{1..4}_*.cast` 与 `output/demo_combined.cast`：
  分段录屏（构建 / 实时切换 / 校验 / 产物），每段带标题、段间 2 s 停顿。

## 关键数字（本次实测，见 verify_report.txt）

| 项目 | 结果 |
| --- | --- |
| 实时/离线输出采样数 | 74406 / 74406，逐采样相等 |
| 切换点 1/2 差分/稳态 p99.9 | 0.019 / 0.044 |
| 单次切换实测耗时 | 0.022 ms / 0.006 ms |
| 4000 次切换最大耗时 | 0.09–0.15 ms（上界证据） |
| 非法率 / 空状态返回码 | 3 `INVALID_ARG` / 2 `BAD_STATE` |
