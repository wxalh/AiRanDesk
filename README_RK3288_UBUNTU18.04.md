# RK3288 Ubuntu 18.04 构建指南

本文档针对 **RK3288 开发板（Firefly 等）运行 Ubuntu 18.04** 系统的特殊构建流程。

> **注意：** RK3288 是 ARMv7 架构（32位），Ubuntu 18.04 较旧，需要手动升级工具链和依赖。

## 系统信息

- **硬件平台：** RK3288 (Rockchip ARM Cortex-A17)
- **架构：** ARMv7-A (32-bit)
- **操作系统：** Ubuntu 18.04 LTS
- **用户：** firefly（根据实际情况修改）

---

## 一、升级系统工具链

Ubuntu 18.04 默认的 GCC/G++ 版本较旧，需要升级到 GCC 10。

### 1.1 安装 GCC/G++ 10

```bash
# 更新软件源并安装 software-properties-common
sudo apt update && sudo apt install -y software-properties-common

# 添加 Ubuntu Toolchain PPA（提供新版本 GCC）
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update

# 安装 GCC 10 和 G++ 10
sudo apt install -y gcc-10 g++-10

# 设置 GCC 10 为默认编译器
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100

# 验证版本
gcc --version
g++ --version
```

### 1.2 安装基础开发工具和依赖库

```bash
# 安装必需的开发工具
sudo apt install -y \
    build-essential \
    pkg-config \
    ninja-build \
    autoconf-archive \
    bison \
    unzip \
    zip \
    nasm \
    meson \
    git

# 安装系统库
sudo apt install -y \
    openssl \
    libssl-dev \
    libdbus-1-dev \
    libxi-dev \
    libxtst-dev \
    libx11-xcb-dev \
    libgl1-mesa-dev \
    libxrender-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    libharfbuzz-dev

# 安装 XCB 相关库（Qt 需要）
sudo apt install -y '^libxcb.*-dev'

# 安装 FFmpeg 开发库
sudo apt install -y \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libavdevice-dev
```

---

## 二、编译安装 Python 3.13

部分构建工具和脚本需要较新版本的 Python 3。

### 2.1 下载并编译 Python 3.13

```bash
# 下载 Python 3.13.9 源码
cd /tmp
wget https://www.python.org/ftp/python/3.13.9/Python-3.13.9.tar.xz
tar -xf Python-3.13.9.tar.xz
cd Python-3.13.9

# 配置编译选项（启用优化和共享库）
./configure \
    --prefix=/usr/local/python-3.13 \
    --enable-optimizations \
    --with-lto \
    --with-computed-gotos \
    --with-system-ffi \
    --enable-shared

# 编译（使用所有 CPU 核心）
make -j$(nproc)

# 安装
sudo make install

# 验证安装
/usr/local/python-3.13/bin/python3 --version
```

---

## 三、编译安装 CMake 3.31

Ubuntu 18.04 自带的 CMake 版本过旧，需要从源码编译新版本。

### 3.1 下载并编译 CMake

```bash
# 下载 CMake 3.31.10 源码
cd /tmp
wget https://github.com/Kitware/CMake/releases/download/v3.31.10/cmake-3.31.10.tar.gz
tar -xf cmake-3.31.10.tar.gz
cd cmake-3.31.10

# 配置（使用系统自带的编译器）
./bootstrap --parallel=$(nproc)

# 编译
make -j$(nproc)

# 安装（需要 root 权限）
sudo make install

# 验证安装
cmake --version
```

---

## 四、配置系统环境变量

### 4.1 编辑 /etc/profile

```bash
sudo vim /etc/profile
```

### 4.2 添加以下内容到文件末尾

```bash
# ==================== 版本要求 Qt 5.9 以上 ====================
# 设置 Qt 根目录（根据实际安装路径修改）
export QTDIR=/opt/Qt/5.9.9_armhf

# 添加 Qt 工具链到 PATH
export PATH=$QTDIR/bin:$PATH

# 添加 Qt 库路径到 LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$QTDIR/lib:$LD_LIBRARY_PATH

# 添加 Qt 手册路径到 MANPATH
export MANPATH=$QTDIR/man:$MANPATH

# ==================== Python 3.13 配置 ====================
export PATH=/usr/local/python-3.13/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/python-3.13/lib:$LD_LIBRARY_PATH
```

### 4.3 使环境变量生效

```bash
# 重新加载环境变量
source /etc/profile

# 验证配置
echo $QTDIR
echo $PATH | grep Qt
qmake --version
python3 --version
cmake --version
```

---

## 五、安装 Qt 5.9.9（ARMv7/ARM HF）

### 5.1 获取 Qt for ARM

Qt 5.9.9 for ARM 可以通过以下方式获取：

- **选项 A：** 使用厂商提供的 Qt for RK3288
- **选项 B：** 从 [Qt 官方](https://download.qt.io/archive/qt/) 下载源码自行编译
- **选项 C：** 使用社区提供的 ARM 预编译包

将 Qt 安装/解压到 `/opt/Qt/5.9.9_armhf/` 目录。

### 5.2 验证 Qt 安装

```bash
# 检查 qmake
which qmake
qmake --version

# 检查 Qt 模块
ls $QTDIR/lib | grep Qt5

# 测试 Qt 配置
qmake -query
```

---

## 六、克隆项目代码

```bash
# 克隆项目（替换为实际的仓库地址）
cd ~
git clone https://github.com/wxalh/AiRanDesk.git AiRanDesk
cd AiRanDesk

# 初始化子模块（spdlog、libdatachannel）
git submodule update --init --recursive
```

---

## 七、编译项目

### 7.1 使用 CMake Presets（推荐）

```bash
# 配置项目（使用 arm-linux 预设）
cmake --preset arm-linux

# 编译（根据 CPU 核心数调整 -j 参数，RK3288 是 4 核）
cmake --build --preset arm-linux -j$(nproc)
```

### 7.2 传统 CMake 方式

```bash
# 创建构建目录
mkdir -p out/build/arm-linux
cd out/build/arm-linux

# 配置项目
cmake ../../.. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_PREFIX_PATH=$QTDIR

# 编译
make -j$(nproc)
```

### 7.3 编译时间参考

- **RK3288（4核，2GB RAM）：** 约 1-2 小时
- 如果内存不足，建议减少并行任务数（`-j2` 或 `-j1`）

---

## 八、运行程序

```bash
# 进入输出目录
cd out/build/arm-linux

# 运行程序
./AiRanDesk
```

---

## 九、常见问题与解决方案

### 9.1 编译时内存不足（Out of Memory）

**症状：** 编译时系统卡顿或进程被 killed

**解决方案：**

1. **减少并行编译任务数：**
   ```bash
   make -j2  # 或 -j1
   ```

2. **增加交换空间（Swap）：**
   ```bash
   # 创建 2GB 交换文件
   sudo fallocate -l 2G /swapfile
   sudo chmod 600 /swapfile
   sudo mkswap /swapfile
   sudo swapon /swapfile
   
   # 永久生效
   echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
   
   # 查看交换空间
   free -h
   ```

### 9.2 找不到 Qt 模块

**症状：** `CMake Error: Could not find Qt5Core`

**解决方案：**

1. 检查环境变量：
   ```bash
   echo $QTDIR
   echo $CMAKE_PREFIX_PATH
   ```

2. 显式指定 Qt 路径：
   ```bash
   cmake --preset arm-linux -DCMAKE_PREFIX_PATH=/opt/Qt/5.9.9_armhf
   ```

### 9.3 OpenSSL 版本不兼容

**症状：** libdatachannel 编译失败，提示 OpenSSL 相关错误

**解决方案：**

项目已配置使用系统 OpenSSL 1.1.1，确保已安装：
```bash
openssl version
# 应该输出 OpenSSL 1.1.1x

sudo apt install -y libssl-dev
```

### 9.4 FFmpeg 链接错误

**症状：** 链接时找不到 `libavcodec`、`libavformat` 等

**解决方案：**

```bash
# 检查 FFmpeg 开发库是否安装
dpkg -l | grep libav

# 重新安装 FFmpeg 开发库
sudo apt install -y \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libavdevice-dev
```

### 9.5 子模块未正确初始化

**症状：** `fatal error: spdlog/spdlog.h: No such file or directory`

**解决方案：**

```bash
# 强制更新子模块
git submodule update --init --recursive --force

# 清空构建缓存后重新编译
rm -rf out/build/arm-linux
cmake --preset arm-linux
```

### 9.6 Python 共享库找不到

**症状：** 运行 Python 时提示 `error while loading shared libraries: libpython3.13.so.1.0`

**解决方案：**

```bash
# 更新动态链接库缓存
sudo ldconfig

# 或者显式添加库路径
export LD_LIBRARY_PATH=/usr/local/python-3.13/lib:$LD_LIBRARY_PATH
```

---

## 十、性能优化建议

### 10.1 编译器优化标志

在 `CMakeLists.txt` 中可以添加 RK3288 特定优化：

```cmake
# RK3288 使用 ARM Cortex-A17，支持 NEON SIMD
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm.*")
    add_compile_options(
        -march=armv7-a
        -mtune=cortex-a17
        -mfpu=neon-vfpv4
        -mfloat-abi=hard
        -O3
    )
endif()
```

### 10.2 运行时优化

```bash
# 设置 CPU 性能模式（需要 root 权限）
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 启用所有 CPU 核心
export OMP_NUM_THREADS=4
```

---

## 十一、参考资源

- [RK3288 官方文档](http://opensource.rock-chips.com/wiki_Main_Page)
- [Qt for Embedded Linux](https://doc.qt.io/qt-5/embedded-linux.html)
- [FFmpeg ARM 优化指南](https://trac.ffmpeg.org/wiki/CompilationGuide/ARM)
- [Ubuntu 18.04 升级指南](https://wiki.ubuntu.com/BionicBeaver/ReleaseNotes)

---

## 十二、构建成功后

编译完成后，请参考主 [README.md](README.md) 文档进行配置和使用：

- 配置 `config.ini` 中的信令服务器地址
- 了解程序的功能特性和使用方法
- 查看故障排除指南

---

**祝编译顺利！如有问题，请提交 Issue。**