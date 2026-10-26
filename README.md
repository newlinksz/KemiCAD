<div align="center">

<img src="docs/kemi/images/kemi-eda-logo.png" alt="KemiCAD" width="128" height="128">

# KemiCAD

**专业、易用的国产化电路设计软件 —— 原理图 · PCB · 3D · 生产输出，一站完成**

[![Release](https://img.shields.io/github/v/release/newlinksz/KemiCAD?label=%E6%9C%80%E6%96%B0%E7%89%88%E6%9C%AC&color=2b7bd6)](https://github.com/newlinksz/KemiCAD/releases/latest)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE.GPLv3)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d6)](https://github.com/newlinksz/KemiCAD/releases/latest)

[⬇️ 下载](https://github.com/newlinksz/KemiCAD/releases/latest) ·
[📖 入门指南](docs/kemi/入门指南.md) ·
[❓ 使用帮助](docs/kemi/使用帮助.md) ·
[📚 Wiki](https://github.com/newlinksz/KemiCAD/wiki) ·
[🐞 问题反馈](https://github.com/newlinksz/KemiCAD/issues)

</div>

---

## 关于 KemiCAD

KemiCAD 是一套完整的电子设计自动化（EDA）软件，覆盖从原理图设计、PCB 布局布线到 3D 查看和生产文件输出的全流程。默认中文界面，预置 KemiCAD 元件库，安装即可开始设计。

## ✨ 主要功能

### 🔌 原理图设计
- 符号放置、连线、总线、网络标签、电源符号
- 层次原理图，适合大型设计分模块管理
- 自动批注、电气规则检查（ERC）
- 导出物料清单（BOM）、网表、PDF / SVG 原理图

### 🧩 PCB 设计
- 多层板设计，交互式布线与差分对布线
- 铺铜、过孔、板框与禁布区
- 电路板配置：层叠、线宽、间距、网络类规则
- 设计规则检查（DRC），原理图与 PCB 手动同步（反向更新仅限受支持的属性）

### 🧊 3D 查看
- 3D 预览电路板与元件（元件模型需另行安装并在「设置 → 配置路径」中配置）
- 在 PCB 编辑器「文件 → 导出」中导出 STEP 等 3D 格式，便于结构配合

### 📦 KemiCAD 元件库
- 安装包内置 KemiCAD 符号库和封装库，开箱即用
- 符号编辑器、封装编辑器，轻松创建自有元件
- 全局库 / 工程库灵活管理

### 🏭 生产文件输出
- Gerber、钻孔文件、元件坐标（贴片）文件
- Gerber 查看器，交板前自检

### 🧰 更多工具
- 插件和内容管理器：安装插件、库与主题
- PCB 计算器、图像转换器、图框编辑器
- 命令行工具 `kicad-cli`，支持批量导出

### 🇨🇳 默认中文界面
- 菜单、对话框默认中文，上手更快

## 💻 系统要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 / 11（64 位） |
| 架构 | x64 |
| 磁盘空间 | 建议预留 1 GB 以上 |

## 📥 安装

1. 打开 [Releases](https://github.com/newlinksz/KemiCAD/releases/latest)，下载 `KemiCAD-x.y.z-x64-setup.exe`。
2. 运行安装程序（需要管理员权限），默认安装到 `C:\Program Files\KemiCAD`。
3. 从开始菜单启动 **KemiCAD**，按 [入门指南](docs/kemi/入门指南.md) 创建第一个工程。

## 🤝 问题反馈

使用中遇到问题或有建议，欢迎在 [Issues](https://github.com/newlinksz/KemiCAD/issues) 提交；软件内「帮助 → 报告 Bug」会自动附带版本信息。

## 📜 开源与致谢

KemiCAD 基于 [KiCad](https://github.com/KiCad) 开源软件开发，感谢 KiCad 开发者和贡献者。本项目按照 GNU 通用公共许可证（GPL）发布，详见 [LICENSE.GPLv3](LICENSE.GPLv3) 及仓库中的其他许可文件。

## ©️ 版权

修改部分版权归深圳新智联软件有限公司所有。

---

## English

**KemiCAD** is a complete EDA suite for schematic capture, PCB layout, 3D viewing and manufacturing output, with a Chinese UI by default and the bundled KemiCAD component libraries. Download the Windows x64 installer from [Releases](https://github.com/newlinksz/KemiCAD/releases/latest). Based on the open-source KiCad software and distributed under the GNU GPL.