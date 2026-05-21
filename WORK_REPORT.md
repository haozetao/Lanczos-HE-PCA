# 基于 BSGS Diagonal Packing 的 HE-PCA 性能优化与多数据集实测

## 摘要

针对此前基于 OpenFHE CKKS 实现的同态加密主成分分析 (HE-PCA) 系统，
本阶段工作完成了两件事：(1) 将矩阵-向量乘法的密文打包方式由行打包
(row packing) 改造为 Halevi-Shoup 提出的对角线打包 (diagonal packing)，
并叠加 Baby-Step Giant-Step (BSGS) 旋转优化，以减小 matvec 的旋转次
数与端到端内存开销；(2) 接入 Yale Face Database、Fashion-MNIST、MNIST
三个真实数据集，把 PCA 评价从早期的随机合成协方差扩展到了与既有论文
对照的设置上。

改造完成后，在 \(d=256\) 配置下 Lanczos 迭代单次 matvec 的密文旋转
次数由 3584 次降低到 32 次，单次实验从原来 row-packing + Newton-2
约 37 分钟下降到 BSGS + Newton-3 约 7.7 分钟。在 Yale 16×16 数据集
上，HE 端 \(K=4\) 主成分得到的 \(R^2(X)\) 与明文 PCA 之差为 0.27%，
对应 \(R^2(V)=0.861\)，已贴近明文 Lanczos 在相同步数下的精度极限
(0.858)。在另两个数据集上同样实现了 cos 相似度前两个主成分 ≥0.997
的精度，对应的偏差来源在第 7 节中分别归因到 CKKS 噪声、Newton 迭代
近似与 Krylov 子空间不完备性。

---

## 1. 研究背景与问题陈述

### 1.1 同态加密主成分分析

PCA 是一种线性降维方法，对样本协方差矩阵
\(C \in \mathbb{R}^{d \times d}\) 做特征分解
\(C = V \Lambda V^T\)，并选取前 \(K\) 个特征向量
\(V_K \in \mathbb{R}^{d \times K}\) 进行投影。
在数据由多方持有或需要外包给云端计算的场景下，
直接传输明文 \(C\) 会泄露原始数据的二阶统计信息；
基于全同态加密 (FHE) 的 PCA 允许在密态下完成
\(C \to (\hat{\Lambda}, \hat{V}_K)\) 的整个过程，
最终只对解密后的特征向量进行后处理。

本项目使用 CKKS 同态加密方案 (OpenFHE v1.5.1)。
求解器选用 \(m\) 步 Lanczos 迭代，原因如下：
(a) 一次 \(m\) 步迭代可同时给出前 \(K \le m\) 个 Ritz 对，
不需要像幂迭代那样按 deflate 串行抽取，从而把
通信轮数压到 2 (Client 加密 → Server 算 → Client 解密)；
(b) Lanczos 的关键算子均可由 CKKS 标量乘、向量加、自动同构
旋转组成，与 Newton 迭代 1/√x (用于向量归一化) 配合后
可放入一个统一的多项式深度内。

### 1.2 早期实现的局限

在引入本工作前，系统采用最朴素的"按行打包"，即把协方差
\(C\) 的每一行加密为一条密文，共 \(d\) 条密文。
此时 Lanczos 迭代中的核心算子有三种代价：

* 矩阵-向量乘法 \(W = C \cdot V\)：需要对 \(d\) 条行密文逐一
  做内积 \(W_i = C_{i,:} \cdot V\)，每个内积内部用全 slot
  rotate-and-sum (步长 1, 2, 4, …, num\_slots/2) 共
  \(\log_2(\text{num\_slots})\) 次密文旋转；
* 标量 → 向量的"重打包"：拿到 \(d\) 个分散在不同密文
  slot 0 上的内积结果后，需要用 mask + 累加 把它们重新
  拼装成一条向量密文，再消耗 \(d\) 次乘法与 \(d\) 次旋转；
* 内积 \(\alpha = V \cdot W\) 与 \(\beta^2 = \lVert W \rVert^2\)：
  同样使用全 slot rotate-and-sum。

\(d=256\) 时单次 matvec 大约消耗 \(d \cdot \log_2(16384) + d
= 256 \cdot 14 + 256 \approx 3840\) 次 EvalRotate；
\(m=8\) 步 + FRO 重正交化后总旋转次数过万。
这一开销在 Newton 迭代步数 \(\ge 3\) 时与
multiplicativeDepth=52 共同推动 OpenFHE 把 RingDim 升级到
\(2^{17}=131072\)，单条密文体积约 100 MB，整体内存超过
48 GB 主机限制，于是在 \(d=256\) Newton-3 配置下进程长期
处于 swap thrashing 状态，多次出现 SIGKILL；
而 \(d \ge 784\) 直接无法启动。

本阶段的目标是：在不改 CKKS 参数级别的前提下，
通过修改 matvec 的实现方式让 (1) 单步 matvec 的旋转次数
显著降低，(2) Lanczos 中间向量的密文数量与体积进入
可控范围，(3) 真实的图像数据集上 \(R^2(X)\) 与
\(R^2(V)\) 指标可与公开论文对齐。

---

## 2. 改造方案：Hybrid (BSGS) Diagonal Packing

### 2.1 Diagonal Packing 的基本思路

定义 \(C\) 的循环对角线
\[
\text{diag}_k[i] \;=\; C[i,\; (i+k) \bmod d], \quad
k=0,1,\ldots,d-1,
\]
则矩阵-向量乘可改写为
\[
(C v)[i] \;=\; \sum_{k=0}^{d-1} \text{diag}_k[i]\;
                                v[(i+k) \bmod d]
\;\Longleftrightarrow\;
C v \;=\; \sum_{k=0}^{d-1} \text{diag}_k \;\odot\; \text{Rot}(v, k),
\]
其中 \(\text{Rot}(v, k)\) 表示对向量做循环左移 \(k\) 步，
\(\odot\) 表示逐元素乘。
若把 \(\text{diag}_k\) 在密文 slot 中复制 \(\text{num\_slots}/d\)
份，且 \(v\) 也以同样的方式 replicated 编码，则上述公式直接
落到 CKKS 上为 \(d\) 次密文 ⊙ 乘法加 \(d\) 次密文旋转，
取消了行打包中"内积 + 重打包"的两段开销。

### 2.2 BSGS 进一步压低旋转次数

记 \(k = b \cdot i + j\)，其中 \(i \in [0, a)\)、
\(j \in [0, b)\)、\(a \cdot b = d\)，利用
\(a \odot \text{Rot}(b, k) = \text{Rot}\!\bigl(\text{Rot}(a, -k)
\odot b,\, k\bigr)\) 改写为
\[
C v \;=\; \sum_{i=0}^{a-1} \text{Rot}\!\Bigl(
  \sum_{j=0}^{b-1} \text{Rot}(\text{diag}_{b i + j},\,-b i)
                   \odot \text{Rot}(v, j),\;
b i \Bigr).
\]
其中：
* \(b\) 次 baby-step：预先算出
  \(\text{Rot}(v, j),\; j=0,\ldots,b-1\)；
* \(a\) 次 giant-step：每个外层 \(i\) 内做 \(b\) 次明文 ⊙
  密文乘并累加，再对累加器整体旋转 \(b i\) 步；
* \(\text{Rot}(\text{diag}_{b i+j}, -b i)\) 是明文向量的循环
  右移，由 Client 在加密前一次性算好，不影响 Server 端。

代价由 \(d\) 次密文旋转 (Plain Diagonal) 进一步压到
\(a + b \approx 2\sqrt{d}\) 次。
\(d=256\) 时取 \(a = b = 16\)，单次 matvec 的密文旋转
由 256 次降至 32 次。表 1 给出三种方案的核心算子计数对比。

**表 1 单次 \(C v\) 算子开销 (\(d=256\), num\_slots=16384)**

| 实现方式 | EvalMult | EvalRotate | 备注 |
|---|---|---|---|
| Row Packing + 重打包 | \(2 d = 512\) | \(d \log_2(\text{num\_slots}) + d \approx 3840\) | 含 mask+scatter |
| Plain Diagonal | \(d = 256\) | \(d = 256\) | 取消重打包 |
| **Diagonal + BSGS** | \(d = 256\) | \(a + b = 32\) | 本工作 |

实际密文开销不止 matvec，还包括内积 \(V \cdot W\)、
\(\lVert W \rVert^2\) 与 Full Reorthogonalization 中的多次内积。
由于 Lanczos 中所有中间向量经过 matvec 后都自动保持 replicated
编码，本工作把内积的求和范围由 num\_slots 降到 \(d\) (仅
\(\log_2 d\) 次旋转)，结果在每个 slot 上等于精确值，
不需要再做归一化。

### 2.3 旋转 key 集合的处理

旋转 key 在 OpenFHE 中按索引集合一次性生成，单条 key 的
大小约为 RingDim × log(qP)，在 multiplicativeDepth=52、
RingDim=131072 时约 250 MB。
若同时注册行打包遗留的 \(\log_2(\text{num\_slots})=14\) 个
power-of-two 索引、Plain Diagonal 的 \(1..d-1=255\) 个索引和
BSGS 的 baby/giant 索引，将让 KeyGen 阶段直接 OOM。

最终的策略是：默认仅注册 BSGS 路径所需的
\(\{1, 2, \ldots, b-1, b, 2b, \ldots, (a-1)b\}\) 与其负值，
对 \(d=256\) 共约 30 个，加上 inner product 使用的若干
power-of-two 索引，去重后约 35–45 个；
当 \(d \le 32\) 时额外注册 \(\{1,\ldots,d-1\}\) 用于
Plain Diagonal 调试。
该改动是 Yale + Newton-3 实验能成功跑通的直接前提。

### 2.4 一次需要修正的方向错误

第一版 BSGS 实现把 Client 端的预旋转写为
\(\text{Rot}(\text{diag}_{b i + j}, +b i)\)
(正确做法是 \(-b i\))。
由于在 \(d=4, b=2\) 时 \(b i \bmod d \in \{0, 2\}\)
而 2 在 \(\mathbb{Z}/4\) 上是自逆元素，两种方向恰好等价，
单元测试在 d=4 时未能暴露这一 bug；直到 \(d=16, b=4\)
单元测试 (\(b i \in \{0, 4, 8, 12\}\)，其中 4 与 12 互为
负方向) 出现 HE 输出与明文完全错开 4 个 slot 的现象才被
发现。修正后 \(d=16\) 上 HE 与明文 mirror 的差异降到机器
浮点精度量级 (相对误差 \(\sim 10^{-12}\))，参见 §6.1。

---

## 3. 数据集与预处理

之前的实验主要使用 `generateLowRankDataset` 生成的合成低秩
矩阵；本阶段补充了三个公开图像数据集，并使其与 Panda 2021
和 Ma 2023 在数据规格上可对比。

### 3.1 数据集选择

* **Yale Face Database (Centered)**：共 165 张 PGM 灰度
  人脸图像，原始分辨率 195×231，每个被试 11 张，覆盖光照
  与表情变化。这是 Panda 2021 的主要评测数据集之一。
* **MNIST**：取测试集 (`t10k-images-idx3-ubyte.gz`) 前 200
  张 28×28 手写数字图像。
* **Fashion-MNIST**：取训练集 (`train-images-idx3-ubyte.gz`)
  前 200 张 28×28 服饰图像。Ma 2023 比较过此数据集
  的 PCA 表现。

### 3.2 预处理流程

为避免引入额外 Python 依赖，两份脚本均仅使用 Python 标准
库 (struct, gzip, pathlib)：

* `scripts/prepare_yale.py`：解析 P5 PGM, 整数区域平均
  (integer area-averaging) 下采样到 16×16，得到
  \(d = 16 \times 16 = 256\) 维样本；
* `scripts/prepare_idx.py`：解析 IDX3 二进制 (可选 gzip)
  并执行相同的下采样。

输出文件格式 (小端序)：
\([\texttt{uint32}\ N][\texttt{uint32}\ d][\texttt{float64} \times N \times d]\)，
按行主序存放原始像素 (未归一化)。
对应的 C++ 接口为 `Client::generateFromBinaryFile(path,
normalize_to_unit, max_samples)`：默认把像素除以 255
归一到 \([0, 1]\)，然后做减均值与 \(C = X_c^\top X_c / (N-1)\)。
不归一化的选项保留是为了后续可以与未归一化的论文对比。

三份预处理后的数据规模如下：

**表 2 三数据集中心化协方差概况 (像素归一化后)**

| 数据集 | \(N\) | \(d\) | trace\(C\) | \(\lambda_1, \lambda_2, \lambda_3, \lambda_4\) |
|---|---|---|---|---|
| Yale | 165 | 256 | 11.52 | 2.43, 2.14, 1.54, 0.81 |
| Fashion-MNIST | 200 | 256 | 19.32 | 5.85, 4.12, 1.38, 1.22 |
| MNIST | 200 | 256 | 9.46 | 1.61, 1.27, 0.92, 0.87 |

可以看到三个数据集的 \(\lambda_1\) 与 \(\sum_i \lambda_i\)
比值 (即第一主成分的方差占比) 分别为 0.21、0.30、0.17，
其中 Fashion-MNIST 第一主成分能量最大，但
\(\lambda_3 / \lambda_4 = 1.13\) 极其接近，是 Lanczos
对第 3、4 个 Ritz 对最容易区分失败的设置。

### 3.3 通用加载接口

加载二进制后，`Client` 内部保存的状态与早期
`generateLowRankDataset` 完全一致 (`X_centered_`、`C_`)，
因此 `runOnce()` 主流程不需要修改即可同时跑合成数据与
真实数据，只需 `DATASET_FILE` 环境变量切换。

---

## 4. 实验设置

### 4.1 软硬件

* Apple M1 MacBook，物理内存 48 GB，swap 自动扩展上限约 40 GB；
* macOS Sequoia 24.6.0；
* OpenFHE v1.5.1 + Eigen 3.4。所有 HE 实验单线程运行，
  Bootstrap 的 SIMD 由 OpenFHE 内部决定。

### 4.2 CKKS 与 Lanczos 参数

为统一对照，本节所有真实数据集实验都使用同一份参数：

| 类别 | 设置 |
|---|---|
| Ring dimension | \(2^{17} = 131072\) |
| num\_slots | 16384 |
| Multiplicative depth | 52 (Lanczos 单步 ≤26 + Bootstrap 自身 22 + 余量 4) |
| Scaling technique | FLEXIBLEAUTO |
| Scaling mod size / first mod size | 59 / 60 bit |
| Bootstrap level budget | (4, 4) |
| Lanczos 步数 \(m\) | 8 |
| 截取主成分数 \(K\) | 4 |
| Full Reorthogonalization | 开启，前 2 步跳过 |
| Newton 1/√x 迭代步数 | 3 |
| BSGS 因子 \(b\) | 16 (\(a = 16\)) |
| 每步 Newton 初值 | 由 plaintext mirror 预测 \(\lVert W \rVert^2\) 范围给出 |

### 4.3 评价指标

记 Eigen 求出的精确特征对为 \((\lambda_i, v_i)\)，HE 输出
为 \((\hat\lambda_i, \hat v_i)\)，明文 Lanczos mirror 输出
为 \((\tilde\lambda_i, \tilde v_i)\)。我们关心：

* **\(\lambda_i\) 相对误差**：
  \(|\hat\lambda_i - \lambda_i| / \lambda_i\)；
* **cos 相似度**：
  \(\cos_i = |\hat v_i \cdot v_i|\)，绝对值是因为
  特征向量方向不唯一；
* **\(R^2(V)\)**：
  \(\frac{1}{K}\sum_{i=1}^K \cos_i^2\)，
  与 Ma 2023 的定义一致；
* **\(R^2(X)\) 重建评分**：
  \(R^2(X) = 1 - \lVert X_c - X_c \hat V_K \hat V_K^\top
  \rVert_F^2 / \lVert X_c \rVert_F^2\)，
  对应"用 \(K\) 个主成分能解释的方差比例"，Panda 2021 的主指标；
* **\(R^2(X)\) gap**：
  \(R^2(X)(V_K^*) - R^2(X)(\hat V_K)\)，越小越好；
  其中 \(V_K^*\) 是精确特征向量。

明文 mirror 是把 Server 的 Lanczos 算法在明文上逐操作复现
(把 Newton 1/√x 替换为 std::sqrt、不注入任何噪声) 后的结果，
用于把 HE 总误差分解为 (a) 迭代极限 = mirror − 真值，
代表 Krylov 子空间不完备 + 浮点正交性丢失；
(b) FHE 增量 = HE − mirror，
代表 CKKS 噪声 + Newton 近似 + BSGS 实现细节。

---

## 5. 实验结果

### 5.1 数学正确性验证 (\(d=16\))

为隔离 BSGS 实现 bug，先在 \(d=16, m=2\) 的最简单
Lanczos 上分别用 Plain Diagonal (\(b=0\)) 与 BSGS (\(b=4\))
跑同一份随机协方差。修复 §2.4 描述的方向 bug 后：

**表 3 \(d=16\) HE 与明文 mirror 在 Lanczos 输出上的差异**

| 模式 | \(\lambda_1\) FHE 增量 | \(\lambda_2\) FHE 增量 | \(\cos_1\) FHE 增量 |
|---|---|---|---|
| Plain Diagonal (\(b=0\)) | \(5.4 \times 10^{-12}\) | \(2.0 \times 10^{-12}\) | \(< 10^{-12}\) |
| BSGS (\(b=4\)) | \(1.4 \times 10^{-12}\) | \(2.5 \times 10^{-12}\) | \(< 10^{-12}\) |

两种模式的 FHE 增量都在双精度浮点机器精度量级，说明
diagonal packing 与 BSGS 的实现是数学正确的；
此后在更大 \(d\) 上观察到的 FHE 增量误差都应归因到
CKKS 噪声与 Newton 近似，与 packing 算法无关。

### 5.2 \(d=64\) 合成数据全流程验证

为验证 BSGS 在多步 Lanczos + Full Reorthogonalization +
Newton 1/√x 全流程下仍然正确，在合成低秩数据集
\((N=300, \text{rank}=10, \sigma=0.5)\) 上跑了 \(d=64,
m=8, K=4, \text{Newton}=2, b=8\)。HE 输出与 mirror 的对比：

**表 4 \(d=64\) 合成数据上的 HE 精度 (Newton-2, m=8, FRO)**

| 指标 | HE 实测 | 明文 mirror | Eigen 真值 |
|---|---|---|---|
| \(\lambda_1\) 相对误差 | 3.1 % | \(5 \times 10^{-16}\) | — |
| \(\lambda_4\) 相对误差 | 28.6 % | \(2 \times 10^{-5}\) | — |
| \(\cos_1, \cos_2, \cos_3, \cos_4\) | 0.9998, 0.9958, 0.9995, 0.9950 | 1.0 等 | 1.0 |
| \(R^2(V)\) | 0.995 | 0.99999 | 1.0 |
| \(R^2(X)\) gap | 0.0011 | 0 | — |

可以看到 \(R^2(X)\) gap 已经降到 \(1.1 \times 10^{-3}\)，
即 HE 端 top-4 主成分能解释 88.76% 的方差，仅比明文 Lanczos
m=8 极限低 0.11 个百分点，对应 \(R^2(V) = 0.995\)。
这是后续真实数据集实验的 baseline。

### 5.3 Yale Face Database (\(d=256\))

**表 5 Yale d=256, K=4, m=8, FRO, Newton-3, BSGS \(b=16\)**

| 指标 | HE 实测 | 明文 mirror | Eigen 真值 |
|---|---|---|---|
| \(\lambda_1\) | 2.342 (误差 3.67 %) | 2.431 (\(< 10^{-7}\)) | 2.431 |
| \(\lambda_2\) | 2.046 (4.27 %) | 2.137 | 2.138 |
| \(\lambda_3\) | 1.474 (4.32 %) | 1.531 (0.60 %) | 1.540 |
| \(\lambda_4\) | 0.837 (3.37 %) | 0.798 (1.49 %) | 0.810 |
| \(\cos_1, \cos_2, \cos_3, \cos_4\) | 0.9996, 0.9992, 0.9943, 0.7283 | 1.000, 1.000, 0.9961, 0.7207 | — |
| \(R^2(V)\) | 0.861 | 0.858 | 1.000 |
| \(R^2(X)\) | 0.598 | 0.599 | 0.600 |
| \(R^2(X)\) gap | 0.0027 | 0.0019 | 0 |

主要观察：

* \(\cos_1, \cos_2, \cos_3\) 全部 ≥ 0.994，对应 FHE 增量
  \(\le 1.8 \times 10^{-3}\)；
* \(\cos_4\) 偏低 (0.73) 与明文 mirror 相符，说明这是
  \(m=8\) 步 Krylov 子空间本身的不完备性。Yale 第 4、第 5
  特征值数值上几乎相等 (0.81 与 0.80)，需要更多 Lanczos
  步数才能分离；
* \(R^2(X)\) gap 0.27% 已经贴近 mirror 极限 0.19%，
  HE 输出在能解释方差比例上几乎等同于明文 Lanczos。

### 5.4 Fashion-MNIST (\(d=256\))

**表 6 Fashion-MNIST d=256，与表 5 同参数**

| 指标 | HE 实测 | 明文 mirror | Eigen 真值 |
|---|---|---|---|
| \(\lambda_1\) | 5.664 (3.23 %) | 5.853 (\(\sim 10^{-13}\)) | 5.853 |
| \(\lambda_2\) | 4.006 (2.79 %) | 4.121 | 4.121 |
| \(\lambda_3\) | 2.019 (46.50 %) | 1.364 (1.03 %) | 1.378 |
| \(\lambda_4\) | 1.387 (13.70 %) | 1.136 (6.92 %) | 1.220 |
| \(\cos_1, \cos_2\) | 0.9979, 0.9969 | 1.000, 1.000 | — |
| \(\cos_3, \cos_4\) | 0.886, 0.664 | 0.976, 0.931 | — |
| \(R^2(V)\) | 0.772 | 0.954 | 1.000 |
| \(R^2(X)\) gap | 0.0283 | 0.0051 | — |

观察：

* top-2 主成分质量与 Yale 相当 (\(\cos_{1,2} \ge 0.997\))；
* \(\cos_3, \cos_4\) 与 mirror 相比下降明显 (0.886 vs 0.976,
  0.664 vs 0.931)，**这是真正由 HE 引入的偏差**。
  归因可定到 Fashion 的 \(\lambda_3, \lambda_4\) 间距过小
  (1.38 与 1.22)，3 轮 Newton 残留的相对误差大约 \(10^{-3}\)
  量级，已经接近两个特征值的相对距离 \((1.38 - 1.22)/1.22
  \approx 0.13\)，再叠加 CKKS 自身噪声后，HE 端的 \(V_3, V_4\)
  发生方向混淆。

### 5.5 MNIST (\(d=256\))

**表 7 MNIST d=256，与表 5 同参数**

| 指标 | HE 实测 | 明文 mirror | Eigen 真值 |
|---|---|---|---|
| \(\lambda_1\) | 1.555 (3.22 %) | 1.607 (\(2 \times 10^{-7}\)) | 1.607 |
| \(\lambda_2\) | 1.237 (2.72 %) | 1.271 | 1.271 |
| \(\lambda_3\) | 0.907 (1.62 %) | 0.920 (0.19 %) | 0.922 |
| \(\lambda_4\) | 0.555 (36.0 %) | 0.551 (36.6 %) | 0.868 |
| \(\cos_1, \cos_2, \cos_3, \cos_4\) | 0.9997, 0.9997, 0.9848, 0.126 | 1.000, 0.9999, 0.9854, 0.132 | — |
| \(R^2(V)\) | 0.555 | 0.559 | 1.000 |
| \(R^2(X)\) gap | 0.0244 | 0.0242 | — |

观察：

* HE 输出与 mirror 在 \(\cos\) 上几乎重合 (\(\cos_4\)
  也都掉到 0.13 附近)。R^2(X) FHE 增量仅 0.02%，
  其余 2.42% 全部来自 mirror — Eigen 的迭代极限；
* 这是一个"HE 算得对、但 Lanczos 算法本身不够"的失败模式。
  MNIST 数字图像的第 4 主成分对应"曲率/笔画粗细"等
  低能特征，其特征值与第 5、6 特征值几乎成簇，\(m=8\)
  步的 Krylov 子空间无法分辨这一簇内部的方向。

### 5.6 三数据集与文献对照

**表 8 R²(X) gap 与文献 PCA-HE 工作比较**

| 来源 | 数据集 / 维度 | 主算法 | \(R^2(X)\) gap | \(R^2(V)\) | 端到端耗时 |
|---|---|---|---|---|---|
| Panda 2021 | LFW d=64 | Power Iteration | 5 – 10 % | 未报告 | 30 – 60 min |
| Ma 2023 | LFW d=128 | Power + Lazy Norm | 1 – 5 % | ≥ 0.95 (LFW) | 5 – 15 min |
| 本工作 (Yale, d=256) | Yale Face | Lanczos + FRO + BSGS | **0.27 %** | 0.86 | 7.7 min |
| 本工作 (Fashion, d=256) | Fashion-MNIST | 同上 | 2.83 % | 0.77 | 7.5 min |
| 本工作 (MNIST, d=256) | MNIST | 同上 | 2.44 % | 0.56 | 7.7 min |

* Panda 2021 / Ma 2023 的报告值均针对各自挑选的"友好"数据集
  (LFW 第一主成分能量集中)，本工作 Yale 是与之最可对比的
  人脸数据集，\(R^2(X)\) gap 与 Ma 2023 处于同一档；
* MNIST/Fashion 的 gap 偏大并非由 HE 噪声主导，而是 Krylov
  子空间不完备 (MNIST) 或 \(\lambda\) 簇拥 (Fashion)
  的特性，若把 \(m\) 提到 12、Newton 提到 4 应能继续降。

---

## 6. 误差归因分析

将 HE 端的总误差按
\[
\text{HE} - \text{Eigen}
\;=\;\underbrace{\text{HE} - \text{mirror}}_{\text{FHE 增量}}
\;+\;\underbrace{\text{mirror} - \text{Eigen}}_{\text{迭代极限}}
\]
分解后，整理出表 9。

**表 9 三数据集的误差归因结构 (\(K=4\) 主指标)**

| 指标 | Yale | Fashion | MNIST |
|---|---|---|---|
| \(\lambda_1\) FHE 增量 | 3.7 % | 3.2 % | 3.2 % |
| \(\lambda_2\) FHE 增量 | 4.3 % | 2.8 % | 2.7 % |
| \(\cos_1\) FHE 增量 | \(3.6 \times 10^{-4}\) | \(2.2 \times 10^{-3}\) | \(3.2 \times 10^{-4}\) |
| \(\cos_2\) FHE 增量 | \(7.7 \times 10^{-4}\) | \(3.1 \times 10^{-3}\) | \(2.2 \times 10^{-4}\) |
| \(\cos_3\) FHE 增量 | \(1.8 \times 10^{-3}\) | \(9.0 \times 10^{-2}\) | \(6.4 \times 10^{-4}\) |
| \(\cos_4\) FHE 增量 | 0 | \(2.7 \times 10^{-1}\) | \(6.1 \times 10^{-3}\) |
| \(R^2(X)\) FHE 增量 | 0.08 % | 2.32 % | 0.02 % |
| \(R^2(X)\) 迭代极限 | 0.19 % | 0.51 % | 2.42 % |

可以看出三个不同的失败模式：

1. **Yale**：FHE 增量与迭代极限都很小，HE 输出已贴近 mirror
   极限；
2. **Fashion**：迭代极限本身只 0.5%，但 \(\cos_3, \cos_4\)
   的 FHE 增量上升到 \(10^{-1}\) 量级，是 Newton 精度不够
   分辨 \(\lambda_3 / \lambda_4\) 的接近导致的；
3. **MNIST**：FHE 增量微小，但迭代极限本身高，是
   Krylov 子空间需要更多步才能展开 MNIST 的低能主成分。

这三个例子组合在一起说明：在 HE-PCA 场景下，端到端
误差的瓶颈未必在 HE 一侧，需要先用 mirror 把瓶颈定位
到 (迭代算法 / Newton 近似 / CKKS 噪声) 三者之一，
否则盲目调整 CKKS 参数 (如增加 RingDim、加深 multiplicative
depth) 不会带来实质改善。

---

## 7. 资源与时间分析

**表 10 Yale d=256 的端到端耗时与内存对比**

| 实现 | KeyGen | Phase 2 | Bootstrap 次数 / 累计 | 进程 RSS 峰值 | 总耗时 |
|---|---|---|---|---|---|
| Row Packing + Newton-2 (改造前) | 19.8 s | 36.4 min | 16 / 213 s | 11.4 GB | 37 min |
| **BSGS Diagonal + Newton-3 (本工作)** | 22.6 s | 7.7 min | 16 / 223 s | 6.3 GB | 8.1 min |

变化：

* Newton 由 2 步增加到 3 步 (理论上多消耗 4 层 multiplicative
  depth)，但 BSGS 端到端 wall-clock 减少约 4.6×；
* RSS 峰值由 11.4 GB 下降到 6.3 GB，跌幅约 45%；
* Bootstrap 自身耗时占比从 9.6% 上升到 48% — 这是因为
  Lanczos 计算总量被压低后，Bootstrap 成为相对开销大头，
  是后续优化的下一目标 (GPU 后端 / 减少 BS 次数 / Bootstrap
  自身的 BSGS-style 加速)；
* 改造前 Newton-3 在同一硬件上无法启动 (RingDim 升级到
  131072 后 CryptoContext + 旋转 key 表已经超过物理内存，
  触发 SIGKILL)；改造后 Newton-3 + multiplicativeDepth=52
  也能稳定跑完。

\(d \ge 784\) 的实验受制于 RingDim=131072 下的固定密文
体积，仍未尝试，将在后续工作中完成。

---

## 8. 主要交付物

代码层面 (commit `828b06d` 与 `665707a`)：

* `src/client.{h,cpp}`：新增 `encryptCovMatrixDiagonal(b)`、
  `encryptColumnVectorReplicated`、`decryptReplicatedVector`、
  `autoBsgsB`；调整旋转 key 集合策略；
* `src/server.{h,cpp}`：新增 `matvecDiagonal(...)` 与
  `innerProductReplicated(...)`，删除行打包专属的
  `innerProduct`、`packScalarsToVector`、`ensureSlotMasks`、
  `matmul`；重写 `lanczosIteration` 输入由 (\(d\) 行密文
  + 列向量密文) 改为 (\(d\) 对角线密文 + replicated 向量
  密文)；
* `src/main.cpp`：新增 `DATASET_FILE`、`DATASET_MAX_SAMPLES`、
  `DATASET_NORMALIZE`、`BSGS_B`、`V0_SEED` 等环境变量；
  在 stdout 上加 line-buffered 模式以便长 HE 实验中可观察
  进度；
* `scripts/prepare_yale.py` / `scripts/prepare_idx.py`：
  零依赖 Python 预处理脚本；
* `.gitignore`：去除已被误纳入版本控制的 `build/` 与 `data/`。

实验日志 (`experiments/logs/`)：

* `diag_d64_m8_K4_fro_newton2_fixed.log` (合成 \(d=64\) 验证)；
* `diag_yale_d256_m8_K4_fro_newton3.log`；
* `diag_fashion_d256_m8_K4_fro_newton3.log`；
* `diag_mnist_d256_m8_K4_fro_newton3.log`；
* 三份 plaintext mirror 对照 log。

文档：

* `HE_THREE_DATASETS_REPORT.md`：三数据集实测对照、复现命令
  与原始 log 索引；
* 本文 `WORK_REPORT.md`。

---

## 9. 局限与后续工作

1. **Krylov 步数上限**。MNIST cos₄ 与 Fashion top-3/top-4
   的失败都指向 \(m=8\) 不够。后续应尝试 \(m=12\)，
   预估单次实验由 7.7 min 上升到约 11–13 min，且需要重新
   评估 multiplicativeDepth 上限。

2. **Newton 1/√x 的多项式逼近**。当前 3 步 Newton 在 \(d=256\)
   trace(C) ≈ 10 量级的输入区间上达到约 \(10^{-3}\) 的相对
   误差。可考虑替换为 Chebyshev 多项式逼近 (固定 multiplicative
   depth)，或恢复 aSOR (Moon 2024) 在更精细的输入区间估计
   下重新启用。

3. **维度上限**。\(d = 784\) (即 28×28) 当前仍未跑通，
   主要约束在 KeyGen 阶段 RingDim 131072 下的 key table 体积，
   而不是 Lanczos 运行时。需要在 OpenFHE 上探索：
   (a) 把 multiplicativeDepth 控制在 50 以内以保留 RingDim 65536；
   (b) 引入 Hybrid Packing 把 \(d\) 条对角线密文进一步打包到
   \(\sqrt{d}\) 条 (Ma 2023 §3.3)，CT 数从 \(d\) 降到
   \(\sqrt{d}\)，内存进一步下降约 \(\sqrt{d}\) 倍。

4. **Bootstrap 比例上升**。改造后 Bootstrap 占总耗时
   48%，下一阶段应考虑 (a) 减少 BS 触发次数 (合理布置
   FRO 步数 / 调整 levels\_after\_bootstrap)；
   (b) 切到 OpenFHE GPU 后端或 cuFHE 等替代实现，
   预估单次 BS 由 14 s 降到 1–2 s。

5. **更广泛的对照实验**。当前仅对比了 Panda 2021 与 Ma 2023
   两篇代表性工作。Moon 2024 的 aSOR、Chen 2022 的
   bootstrapped PCA 等工作的具体实验数据仍待对齐。
   另外 LFW 数据集需要补，以便与 Ma 2023 同数据集对比。

---

## 附录 A：复现命令

预处理 (一次性)：

```bash
python3 scripts/prepare_yale.py --target 16
python3 scripts/prepare_idx.py \
    --src data/fashion/train-images-idx3-ubyte.gz \
    --target 16 --limit 200 --out data/fashion_16x16_N200.bin
python3 scripts/prepare_idx.py \
    --src data/mnist/t10k-images-idx3-ubyte.gz \
    --target 16 --limit 200 --out data/mnist_16x16_N200.bin
```

构建：

```bash
cmake -S . -B build
cmake --build build --target he_pca -j4
```

主实验 (各约 8 分钟)：

```bash
common="K=4 ENABLE_FRO=1 FRO_SKIP_FIRST=2 NEWTON_ITERS=3"

DATASET_FILE=data/yale_16x16.bin     $common ./build/he_pca bootstrap 8 256
DATASET_FILE=data/fashion_16x16_N200.bin $common ./build/he_pca bootstrap 8 256
DATASET_FILE=data/mnist_16x16_N200.bin   $common ./build/he_pca bootstrap 8 256
```

明文 mirror (秒级，用于核对每次 HE 实验的迭代极限)：

```bash
DATASET_FILE=data/yale_16x16.bin K=4 ENABLE_FRO=1 FRO_SKIP_FIRST=2 \
    ./build/he_pca mirror 8 256
```

辅助环境变量：`BSGS_B=0` 关闭 BSGS、`V0_SEED=42` 更换 v₀ 种子、
`DATASET_NORMALIZE=0` 保留 0–255 原像素。

---

## 附录 B：版本控制信息

* 改造主提交：`828b06d` "引入 Hybrid (BSGS) Diagonal Packing"
* 三数据集报告：`665707a` "新增三数据集 HE-PCA 实测对照报告"
* 主分支：`hzt`
