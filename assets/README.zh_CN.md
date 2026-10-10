<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 赛博像素 UI 字库集

应用界面文字由两个 SIL Open Font License 1.1 字族渲染。两者都是按 8 px 网格绘制的轮廓 TTF，因此在 8 px 的整数倍上渲染能让字形边缘落在像素网格上，保持硬边像素观感。

| 文件 | 字族、字级、位深 | 角色 | 用途 |
| --- | --- | --- | --- |
| [`fonts/silkscreen/Silkscreen-Regular.ttf`](fonts/silkscreen/Silkscreen-Regular.ttf) | Silkscreen Regular，轮廓 TTF | 源文件 | 两个 `cyber_font_*` 界面字级的源字体。 |
| [`fonts/silkscreen/Silkscreen-Bold.ttf`](fonts/silkscreen/Silkscreen-Bold.ttf) | Silkscreen Bold，轮廓 TTF | 源文件 | 预留作强调字重，暂未转换。 |
| [`fonts/pressstart2p/PressStart2P-Regular.ttf`](fonts/pressstart2p/PressStart2P-Regular.ttf) | Press Start 2P Regular，轮廓 TTF | 源文件 | 三个 `cyber_font_title*` / `cyber_font_code` 字级的源字体。 |
| [`fonts/noto-sans-sc/NotoSansSC-VF.ttf`](fonts/noto-sans-sc/NotoSansSC-VF.ttf) | Noto Sans SC（可变字重），轮廓 TTF | 源文件 | 中文字幕字体的源字体。SIL OFL 1.1，见 `OFL.txt`。 |
| [`fonts/generated/cyber_font_sm.c`](fonts/generated/cyber_font_sm.c) | Silkscreen Regular，8 px，4 bpp | 生成物 | 状态栏与提示文字（`cyber_font_sm`）。 |
| [`fonts/generated/cyber_font_md.c`](fonts/generated/cyber_font_md.c) | Silkscreen Regular，16 px，4 bpp | 生成物 | 状态名称与正文（`cyber_font_md`）。AI 字幕改用单独的 `cyber_font_cjk` 字体。 |
| [`fonts/generated/cyber_font_title.c`](fonts/generated/cyber_font_title.c) | Press Start 2P Regular，16 px，4 bpp | 生成物 | 标题与短标签（`cyber_font_title`）。 |
| [`fonts/generated/cyber_font_title_lg.c`](fonts/generated/cyber_font_title_lg.c) | Press Start 2P Regular，24 px，4 bpp | 生成物 | 开机横幅与大号读数（`cyber_font_title_lg`）。 |
| [`fonts/generated/cyber_font_code.c`](fonts/generated/cyber_font_code.c) | Press Start 2P Regular，48 px，4 bpp | 生成物 | 配对码（`cyber_font_code`），界面里最大的一处文字，由激活屏拆成两行显示。 |
| [`fonts/generated/cyber_font_cjk.c`](fonts/generated/cyber_font_cjk.c) | Noto Sans SC，16 px，1 bpp | 生成物 | AI 字幕（`cyber_font_cjk`）；ASCII + 基本汉字区 0x4E00-0x9FFF（约 6.7k 字）。生成命令见 `fonts/generate-fonts.sh` 末尾的 CJK 小节。 |

- **许可。** 两个字族均为 SIL Open Font License 1.1，许可证全文以 `OFL.txt` 与各自源字体放在同一目录。允许再分发与嵌入。
- **转换器。** `lv_font_conv` 1.5.3，产物与 [`../dependencies.lock`](../dependencies.lock) 锁定的 LVGL 9.5.0 兼容。全部重新生成请执行 `./assets/fonts/generate-fonts.sh`。
- **集成方式。** 生成的 `.c` 由 `main` 组件编译，并在其中用 `LV_FONT_DECLARE` 声明；仅把字体文件放进本目录并不会自动链接或生效。
- **字符覆盖。** 四个像素字体仅覆盖可打印 ASCII（`0x20-0x7E`）；界面标签为英文，刻意不含中文字形。AI 字幕是中文，因此改用单独的 `cyber_font_cjk` 字体（ASCII + 基本汉字约 6.7k 字）。超出该范围的字符渲染为 LVGL 占位框而非空白——这是刻意的不支持字符策略，见 [`../docs/development/engineering/coding-conventions.md`](../docs/development/engineering/coding-conventions.md)。
- **`cyber_font_code` 不是通用字体。** 配对码是用户唯一需要"隔着手臂距离从屏幕上读下来、再敲到另一台设备上"的字符串，因此它比其他所有文字都大得多。48 px 下若要覆盖完整可打印 ASCII，为画十个数字就得生成约 140 KB 的 C 文本；所以它的源范围只取空格、`-`、`:` 与 `0-9`。其余字符会**静默地什么都不渲染**——正文一律改用其它字体。
- **资源成本。** 五个像素字体合计约 119 KB 的 C 文本；`cyber_font_cjk` 更大（链接后数 MB，进镜像约 2.5 MB），因为它承载数千个汉字字形，但固件仍保留约 70% 的应用分区余量。字体描述符为 `const` 常驻 Flash，RAM 开销仅剩 LVGL 的单字形绘制缓冲。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
