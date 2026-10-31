# Ti60 2D 图形控制台

基于 Efinix **Ti60F225** FPGA 的 RISC-V SoC 嵌入式图形控制台：Sapphire 软核（RV32IM）搭配自研 **gfx2d 2D 图形加速引擎**，DDR3 帧缓冲，DVI/HDMI 视频输出，一片 FPGA 内固化 **四个可切换镜像**（菜单 / 精灵压测 / Boss 战 / 弹幕）。

## 赛题背景

本项目为 **2026 年全国大学生嵌入式芯片与系统设计竞赛——FPGA 创新设计赛道**参赛作品，选题为易灵思赛题二：**基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎**。

赛题面向 2D 游戏与嵌入式图形界面中 CPU 逐像素搬运开销大的问题，要求通过软硬件协同分担渲染任务：RISC-V 负责高层逻辑与指令下发，FPGA 加速器负责图像块拷贝、纯色填充及像素混合。基础目标包括直接访问外部存储器、突发传输与 FIFO 优化、双缓冲和至少 640×480 @ 60 Hz 显示；高阶挑战包括 Alpha 混合、Color Keying，以及高负载互动游戏中的精灵渲染。

## 小组成员

- [HXHndj](https://github.com/HXHndj)
- [hu9599](https://github.com/hu9599)
- [alley11](https://github.com/alley11)

## 硬件与工具链

| 项 | 说明 |
|---|---|
| 开发板 | Efinix Titanium Ti60F225（DDR3 + DVI/HDMI 输出） |
| FPGA 工具 | Efinity 2026.1（综合 / 布局布线 / 编程） |
| 固件工具 | Efinity RISC-V Embedded IDE 2026.1（RV32IM GCC） |
| SoC | Sapphire RISC-V 软核 + 自研 gfx2d 2D 加速引擎 |

gfx2d 引擎包含命令队列、精灵流水线、桶（bucket）引擎、tile 合成引擎、文本叠加、行回放缓存、扫描 CRC 校验等 RTL 模块，固件通过 MMIO 提交 2D 命令（COPY/FILL/SPRITE/BLEND/TEXT），由硬件完成全部像素级渲染。

## 四镜像

| 目录 | Efinity 工程 | 内容 |
|---|---|---|
| `launch/` | `launch.xml` | 菜单启动器，上电入口，选择进入各镜像 |
| `m12/` | `m12.xml` | Sprite FIELD 精灵压测演示：512…4096 精灵负载档位、多档精灵尺寸，行级预取与双 bank 流水 |
| `key3/` | `key3.xml` | Boss Rush：1920 精灵池版本，640×480 视频输出 |
| `moonshadow/` | `ddr_board.xml` | Moonshadow：弹幕演示，LZ4 压缩 ROM 自举 + tile 引擎 |

每个镜像是独立完整的 Efinity 工程，独享全部 FPGA 资源；`m12` / `key3` 支持四键同按约 600 ms 返回菜单，`moonshadow` 通过实体 CRESET 返回。

M12 已包含 PAD24 冷启动修复：首次初始化默认启用 24×24 填充行缓存优化，后续切换精灵尺寸、负载时保留运行时设置。源码与嵌入 ROM 初始化数据同步更新，无需开机后通过 UART 手动启用。

## 一键构建

```bash
python -m pip install -r requirements.txt
python build.py                       # 全部四个镜像 + 四槽 Flash 组合镜像
python build.py --image key3          # 单个镜像
python build.py --firmware-only       # 仅重编固件
```

- 依赖 Efinity 2026.1 与 Efinity RISC-V IDE，默认路径 `D:/Efinity/2026.1`、`D:/Efinity/efinity-riscv-ide-2026.1`，可用 `--efinity` / `--rv` 或环境变量 `EFINITY_HOME` / `EFINITY_RISCV_HOME` 覆盖。
- 产物输出到 `build/`：各镜像 `.bit` / `.hex` / timing / map 报告，以及 `combined.hex`（官方 multi_image_merger 生成的四槽 Flash 组合镜像）。FPGA 编译在 ASCII 临时路径中进行。

## 手动构建（IDE）

1. 在 Efinity 中打开 `launch/launch.xml`、`m12/m12.xml`、`key3/key3.xml` 或 `moonshadow/ddr_board.xml`；
2. 固件：将对应镜像的 `firmware/` 目录以 *Existing Makefile Project* 导入 Efinity RISC-V IDE，或在命令行运行 `python firmware/build.py`；
3. **修改 C 源码后必须先重编固件再编译 FPGA**——固件以 ROM 形式嵌入位流。

## 烧录

- **JTAG SRAM**：直接烧录各镜像 `build/<image>/<image>.bit`（易失，验证用）；
- **Flash 固化**：Efinity Programmer 的 JTAG bridge 两步法——先烧 Ti60 bridge loader，再烧 `build/combined.hex`，实体 CRESET 重载；CBSEL 00/01/10/11 依次对应 launch / m12 / key3 / moonshadow。

## 目录结构

```
build.py            # 一键构建：4 镜像固件 + FPGA 编译 + 四槽 Flash 组合
requirements.txt    # Python 依赖（lz4）
launch/  m12/  key3/  moonshadow/   # 四个独立 Efinity 工程镜像
```

## License

[MIT](LICENSE)
