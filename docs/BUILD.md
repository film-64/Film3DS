# 构建与打包

## 主机

Ubuntu / Debian 安装 `build-essential libjpeg-dev python3-pil`。如果 Python 来自独立环境，在其虚拟环境中安装 Pillow。

```sh
make -j4 test
make bench
make sanitize
python3 tools/control.py mock --dump build/mock.yuyv
python3 tools/simulator.py
```

模拟器只绑定 `127.0.0.1`，并使用进程管道连接主机 C 核心。在浏览器访问 `http://127.0.0.1:8765`。支持方向键、Enter、Esc 与下屏点击。关闭 Python 进程会清理该次桌面模拟的临时胶卷。主机和 ARM 版本共享 C 算法、导航状态与协议。

## ARM11

按 devkitPro 的 pacman 安装说明配置开发机。已配置 `dkp-pacman` 后执行以下命令，按安装环境添加所需权限，并在主机安装 Git 与 CMake（Ubuntu 可用 `sudo apt-get install git cmake`）。

```sh
dkp-pacman -Syu --needed 3ds-dev
export DEVKITPRO=/opt/devkitpro
make -j4 3ds
python3 tools/verify_3dsx.py Film3DS.3dsx
sh tools/verify_arm11.sh Film3DS.elf "$DEVKITPRO/devkitARM/bin/arm-none-eabi-"
"$DEVKITPRO/devkitARM/bin/arm-none-eabi-readelf" -hA Film3DS.elf
"$DEVKITPRO/tools/bin/3dsxdump" Film3DS.3dsx build-3ds/executable.dump
```

CI 固定官方 devkitARM 镜像的 linux/amd64 manifest digest。其 ARM11 任务上传 `Film3DS.3dsx` 和 SHA-256 及 libjpeg-turbo 许可说明。镜像和工作区路径可能影响构建字节，比较版本时记录镜像、Git SHA 和产物 SHA。ELF 带调试信息，可供 GDB 使用。

```sh
docker run --rm -v "$PWD:/src" -w /src \
  devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b \
  make -j4 3ds
```

`tools/build_arm_jpeg.sh` 获取 libjpeg-turbo 3.1.3 的固定 Git 提交并以 `WITH_SIMD=OFF` 构建到 `build-3ds/`。本轮的 devkitPro JPEG 包会把 NEON 路径链接进 ELF，故 3DS 端使用独立的标量构建。主机继续用系统 libjpeg。首次构建需要网络访问 GitHub。CI 复核 VFPv2、NEON 符号和 ARMv7 屏障指令。

3DSX 使用官方 `3dsx.specs`、`3dsxtool`、`smdhtool` 和工具链默认 Homebrew 图标。构建输入来自 C 源码与工具链，应用没有专有资源依赖。

## CIA 条件

CIA 方案需要 `makerom`、`ctrtool`、有权使用的图标和 banner、为本应用分配且确认无冲突的唯一标题 ID，以及审查过的 RSF。先用匹配 devkitARM 版本的应用启动与链接设置生成独立标题 ELF，再核对 ExHeader 的文件系统权限、CAM/Y2R/GSP 和 APT 等服务访问。3DSX 的 hbloader 环境与独立标题的 ExHeader 配置分别验证。

持有上述输入后，打包命令的形状如下。保留 `Film3DS.elf` 给 3DSX 的调试流程。

```sh
makerom -f cia -o Film3DS.cia -target t -rsf film3ds.rsf \
  -elf Film3DS-title.elf -icon Film3DS.smdh -banner Film3DS.bnr
ctrtool -i Film3DS.cia
```

`film3ds.rsf`、独立标题链接输入、标题 ID 和 banner 尚待确定。本轮交付 3DSX，CIA 打包、安装与启动未运行。工具参数以项目发行版为准。 [Project_CTR 发行版](https://github.com/3DSGuy/Project_CTR/releases) · [makerom 源文档](https://github.com/3DSGuy/Project_CTR/tree/master/makerom)
