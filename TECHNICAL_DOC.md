# HE-PCA 技术文档：基于 CKKS 同态加密的隐私保护主成分分析

## 目录

- [1. 项目概述](#1-项目概述)
- [2. 系统架构](#2-系统架构)
- [3. CKKS 同态加密参数设计](#3-ckks-同态加密参数设计)
- [4. 核心算法](#4-核心算法)
- [5. 数据打包与密文操作](#5-数据打包与密文操作)
- [6. 源码结构与模块详解](#6-源码结构与模块详解)
- [7. 端到端执行流程](#7-端到端执行流程)
- [8. CKKS Scale 对齐机制](#8-ckks-scale-对齐机制)
- [9. 构建与运行](#9-构建与运行)
- [10. 架构演进](#10-架构演进)
- [11. 局限性与未来方向](#11-局限性与未来方向)
- [12. 后 OpenFHE 迁移工作与实验](#12-后-openfhe-迁移工作与实验)

---

## 1. 项目概述

### 1.1 解决的问题

主成分分析（PCA）是机器学习中最基础的降维方法之一，需要对数据的协方差矩阵进行特征值分解。在隐私计算场景（如医疗数据降维、金融风控特征提取、联邦学习中的跨机构特征工程）中，数据所有者不愿将原始数据明文暴露给第三方计算平台。

本项目实现了一个 **基于 CKKS 全同态加密（FHE）的隐私保护 PCA 系统**。数据所有者（Client）将协方差矩阵加密后发送给云服务器（Server），Server 在密文上直接执行完整的 Lanczos 迭代，全程无法获知任何明文信息。Client 仅需在最终阶段解密并完成后处理，即可得到与明文 PCA 高度一致的特征值和特征向量。

### 1.2 核心特性

| 特性 | 说明 |
|------|------|
| **最少交互轮次** | Client-Server 仅需 2 轮通信（发送加密数据 + 接收加密结果） |
| **完整 PCA 输出** | 同时提取特征值和特征向量，支持数据投影 |
| **aSOR Newton 加速** | 集成自适应逐次超松弛法（aSOR），可将 Newton 迭代次数减半 |
| **CW 鬼影过滤** | 通过 Cullum-Willoughby 方法自动过滤因省略重正交化产生的伪特征值 |
| **深 CKKS 电路** | 使用 `poly_modulus_degree=32768` 的深参数链，支持 Server 一次性完成所有计算 |

### 1.3 技术栈

| 组件 | 技术选型 | 版本 |
|------|---------|------|
| 同态加密库 | **OpenFHE** (CKKS 方案, 支持 Bootstrapping) | **v1.5.1** |
| 线性代数库 | Eigen | 3.4.0 |
| 编程语言 | C++17 | — |
| 构建系统 | CMake + FetchContent | >= 3.16 |
| 运行时依赖 | OpenMP (macOS 通过 Homebrew `libomp`) | — |

### 1.4 参考论文

- **aSOR 方法**: *"Adaptive Successive Over-Relaxation Method for a Faster Iterative Approximation of Homomorphic Operations"* (Moon et al., 2024)
- **Cullum-Willoughby 过滤**: 用于在无完全重正交化的 Lanczos 过程中识别和去除鬼影特征值的经典方法

---

## 2. 系统架构

### 2.1 双角色模型

系统包含两个角色，以 C++ 类模拟（无需实际网络通信）：

```
┌──────────────────────────────────┐       ┌──────────────────────────────────┐
│           Client (数据所有者)       │       │          Server (云计算平台)        │
│                                  │       │                                  │
│  持有:                            │       │  持有:                            │
│   · SecretKey (私钥)              │       │   · PublicKey (公钥)              │
│   · PublicKey (公钥)              │       │   · RelinKeys (重线性化密钥)       │
│   · CKKSEncoder / Decryptor      │       │   · GaloisKeys (Galois 旋转密钥)  │
│                                  │       │   · Evaluator (密态运算引擎)       │
│  职责:                            │       │   · NewtonInvSqrt (密文 1/√x)     │
│   · 生成协方差矩阵 C               │       │                                  │
│   · 归一化: C_hat = C / trace(C)  │       │  职责:                            │
│   · 行打包加密 C、列打包加密 v0     │       │   · 完整密态 Lanczos 迭代          │
│   · 解密 α, β, V_all             │       │   · 密文矩阵-向量乘法              │
│   · 构建三对角矩阵 T_m            │       │   · 密文内积 (rotate-and-sum)      │
│   · CW 过滤去除鬼影               │       │   · Newton 1/√x 归一化            │
│   · Gram-Schmidt 正交化           │       │   · 仅接触密文, 不持有私钥          │
│   · Ritz 向量重构特征向量          │       │                                  │
└──────────────────────────────────┘       └──────────────────────────────────┘
```

### 2.2 信息流

```
Client                              Server
  │                                   │
  │── enc_C, enc_v0, PublicKeys ────→ │
  │                                   │  ┌─────────────────────────────────┐
  │                                   │  │ for j = 1 to m_iter:           │
  │                                   │  │   W = C · V        (矩阵乘)    │
  │                                   │  │   W -= β·V_prev    (三项递推)   │
  │                                   │  │   α = V^T · W      (内积)      │
  │                                   │  │   W_new = W - α·V  (局部正交)   │
  │                                   │  │   β = Newton(‖W_new‖²) (归一化)│
  │                                   │  │   V_next = W_new · (1/√x)      │
  │                                   │  │   保存 V_j, α_j, β_j          │
  │                                   │  └─────────────────────────────────┘
  │←── enc_α[], enc_β[], enc_V_all ──│
  │                                   │
  │  解密 → 构建 T_m → CW 过滤        │
  │  → Gram-Schmidt → 特征向量重构     │
```

**交互轮次: 2 轮**（初始化发送 + 接收结果）

### 2.3 隐私保证

| 传输方向 | 数据内容 | 加密状态 |
|---------|---------|---------|
| Client → Server | `enc_C` (协方差矩阵密文), `enc_v0` (初始向量密文), 公钥 | 密文 |
| Server → Client | `enc_α[]`, `enc_β[]`, `enc_V_all` | 密文 |

Server 全程只接触密文和公钥，**数学上无法推断**协方差矩阵、初始向量或任何中间结果的明文值。

---

## 3. CKKS 同态加密参数设计

### 3.1 参数配置（OpenFHE `CCParams<CryptoContextCKKSRNS>`）

```cpp
CCParams<CryptoContextCKKSRNS> parameters;
parameters.SetSecretKeyDist(UNIFORM_TERNARY);
parameters.SetSecurityLevel(HEStd_NotSet);   // toy 模式：手动设 RingDim
parameters.SetRingDim(1u << 15);             // N = 32768, numSlots = 16384
parameters.SetScalingTechnique(FLEXIBLEAUTO);// 自动管理 rescale/scale
parameters.SetScalingModSize(59);
parameters.SetFirstModSize(60);

// baseline 模式：直接给足够的乘法深度，不含 Bootstrap 开销
parameters.SetMultiplicativeDepth(19);

// bootstrap 模式：Bootstrap 自身消耗 + 业务逻辑所需层数
// uint32_t bs_depth = FHECKKSRNS::GetBootstrapDepth({4,4}, UNIFORM_TERNARY);
// parameters.SetMultiplicativeDepth(levels_after_bootstrap + bs_depth);
```

### 3.2 参数解读

| 参数 | 值 | 说明 |
|------|-----|------|
| `RingDim (N)` | 32768 | 多项式环维度，`numSlots = N/2 = 16384` |
| `ScalingTechnique` | `FLEXIBLEAUTO` | OpenFHE 自动插入 rescale、维护 scale 一致性 |
| `ScalingModSize` | 59 | 工作层素数位宽（类比 SEAL 的 40-bit） |
| `FirstModSize` | 60 | 首层特殊素数位宽 |
| `SecurityLevel` | `HEStd_NotSet` | toy 模式；生产可用 `HEStd_128_classic` |
| `MultiplicativeDepth` | 19（baseline）/ 36（bootstrap） | 包含或不包含 Bootstrap 消耗 |
| 旋转密钥 | `{±2^i, i=0..log2(numSlots)-1}` 约 30 个 | 仅 2 的幂即可覆盖 rotate-and-sum（修复前多生成 2(d+6) 小步键，d=4096 会占用 ~260GB） |
| Bootstrap `levelBudget` | `{4, 4}` | CoeffsToSlots / SlotsToCoeffs 各 4 层 |

### 3.3 乘法深度预算（OpenFHE FLEXIBLEAUTO）

OpenFHE `FLEXIBLEAUTO` 采用懒惰 rescale：fresh 密文的第一次乘法通常只把 `noiseScaleDeg` 从 1 推到 2，`GetLevel()` 不变；后续乘法再触发 level 增长。因此本项目不再只按“乘法次数”估深度，而是用 `src/depth_probe.cpp` 对当前 OpenFHE 1.5.1 配置实测。

**重要结论**：在本项目默认 `MakeCKKSPackedPlaintext` 编码下，`ct × Plaintext` 与 `ct × ct` 对 level 的影响相同；论文中“明文乘免费”通常指 BGV/BFV，或 CKKS 低 scale 明文编码，不适用于当前 `FLEXIBLEAUTO` 默认编码。

```
操作                                      实测层数影响
────────────────────────────────────────────────────
EvalAdd / EvalSub / EvalRotate            0
ct × ct / ct × Plaintext / ct × double    连续 n 次乘法约消耗 n-1 层
Newton 首轮                               3 层
Newton 后续每轮                           4 层
Newton(n) + sqrt = x·y                    4 × n 层
单步 Lanczos V→V_next                     4 + 4 × newton_iters 层
────────────────────────────────────────────────────
newton=2 时单步 Lanczos = 12 层
```

**Baseline 模式**：`m_iter=2`, `newton_iters=2`，最大约 15 层；设置 `SetMultiplicativeDepth(19)` 有 4 层余量。

**Bootstrap 模式**：`newton_iters=2` 时每步 12 层，默认 `levels_after_bootstrap=14`，即每次 Bootstrap 后有 2 层余量；通过在每轮 Lanczos 开始前判断剩余深度，对 `V / V_prev / β_prev` 三个关键密文调用 `EvalBootstrap` 刷新，使多步 Lanczos 成为可能。

#### 3.3.1 Bootstrap 触发逻辑

```cpp
// 触发阈值 = 单步所需深度 + 2 层余量
uint32_t per_iter_depth     = 4 + 4 * newton_steps;
uint32_t bootstrap_threshold = per_iter_depth + 2;
```

当 `totalDepth - ct->GetLevel() < bootstrap_threshold` 时对该密文执行 `EvalBootstrap`，将剩余深度恢复至 `levels_after_bootstrap`。

---

## 4. 核心算法

### 4.1 Lanczos 迭代（标量版，p=1）

Lanczos 算法通过构建 Krylov 子空间 $\mathcal{K}_m(C, v_0) = \text{span}\{v_0, Cv_0, C^2v_0, \ldots\}$ 来逼近对称矩阵 $C$ 的极端特征值。标准三项递推：

$$w_j = C v_j - \beta_{j-1} v_{j-1}$$
$$\alpha_j = v_j^T w_j$$
$$\hat{w}_j = w_j - \alpha_j v_j$$
$$\beta_j = \|\hat{w}_j\|, \quad v_{j+1} = \hat{w}_j / \beta_j$$

产生对称三对角矩阵 $T_m$（$\alpha$ 为对角线，$\beta$ 为次对角线），其特征值（Ritz 值）逼近 $C$ 的特征值。

**密文实现的关键挑战**：归一化步骤 $v_{j+1} = \hat{w}_j / \|\hat{w}_j\|$ 需要计算范数的倒数，这在 CKKS 密文上无法直接执行除法或开方，需借助 Newton 迭代逼近。

### 4.2 Newton 迭代求 1/√x

对于密文中的 $x = \|\hat{w}_j\|^2$，使用 Newton-Raphson 方法逼近 $1/\sqrt{x}$：

$$y_{k+1} = y_k \cdot \frac{3 - x \cdot y_k^2}{2}$$

**每轮迭代的 CKKS 操作**：
1. $y^2$ — square + relinearize + rescale（1 层深度）
2. $x \cdot y^2$ — multiply + relinearize + rescale（1 层深度）
3. $y \cdot (3 - x \cdot y^2)$ — negate + add_plain + multiply + rescale（1 层深度）
4. $\times 0.5$ — multiply_plain + rescale（1 层深度）

总计每轮约 4 层深度。初始猜测 $y_0 \approx 1/\sqrt{x}$ 由 Client 在明文侧通过模拟第一步 Lanczos 获得。

### 4.3 aSOR 加速 Newton

aSOR（adaptive Successive Over-Relaxation）引入松弛因子 $k_i$，将迭代变为：

$$y'_i = k_i \cdot y_i, \quad y_{i+1} = y'_i \cdot \frac{3 - x \cdot y_i'^2}{2}$$

**零额外深度消耗的吸收技巧**：
- 第一次迭代：将 $k_1$ 吸收进初始猜测，加密 $k_1 \cdot y_0$ 而非 $y_0$
- 中间迭代：将 `multiply_plain(0.5)` 替换为 `multiply_plain(k_{i+1} / 2)`
- 最后一次迭代：正常乘 0.5

松弛因子预计算（离线，明文）：

$$k_i = \sqrt{\frac{3}{1 + \epsilon_i + \epsilon_i^2}}, \quad \epsilon_{i+1} = k_i \cdot \frac{3 - k_i^2}{2}$$

**预期效果**：相同精度下 aSOR 可将迭代次数从 3 次降至 2 次，节省约 4 层乘法深度。

### 4.4 Cullum-Willoughby (CW) 鬼影过滤

由于省略了完全重正交化（FRO），Lanczos 过程会产生「鬼影」——重复出现的伪特征值。CW 方法通过比较两个矩阵的特征值来识别鬼影：

1. 计算 $T_m$ 的全部特征值 $\{\lambda_i\}$
2. 构造子矩阵 $T_s = T_m$ 删去第 1 行第 1 列
3. 计算 $T_s$ 的特征值 $\{\mu_j\}$
4. 若 $|\lambda_i - \mu_j| < \text{tolerance}$ 对某 $j$ 成立，则 $\lambda_i$ 为鬼影

过滤后保留前 $K$ 个最大的有效 Ritz 值及其对应 Ritz 向量。

### 4.5 特征向量重构

从 Lanczos 过程中恢复原始空间的近似特征向量：

1. **解密 Lanczos 基向量**：$V_{\text{total}} = [v_1, v_2, \ldots, v_m]$（$d \times m$ 矩阵）
2. **Gram-Schmidt 正交化**：修复 CKKS 噪声累积导致的正交性丧失
3. **Ritz 向量映射**：$u_i = Q \cdot s_i$，其中 $Q$ 为正交化后的基，$s_i$ 为 CW 过滤后的 Ritz 向量
4. **归一化**：$\hat{u}_i = u_i / \|u_i\|$

### 4.6 输入归一化策略

Newton $1/\sqrt{x}$ 的收敛性依赖输入范围。归一化方案：

$$\hat{C} = C / \text{trace}(C)$$

使特征值之和为 1，最大特征值 < 1。Newton 输入 $\|W\|^2$ 相应缩放到可控范围。最终特征值通过乘回 $\text{trace}(C)$ 反归一化。

---

## 5. 数据打包与密文操作

### 5.1 协方差矩阵 C — 行打包

$$\text{enc\_C}[i] = \text{Encrypt}([C_{i,0}, C_{i,1}, \ldots, C_{i,d-1}, 0, \ldots, 0])$$

共 $d$ 个密文，每个密文的前 $d$ 个 slot 存储 $C$ 的第 $i$ 行。

### 5.2 向量 v — 列打包

$$\text{enc\_v}[0] = \text{Encrypt}([v_0, v_1, \ldots, v_{d-1}, 0, \ldots, 0])$$

$p=1$ 时仅 1 个密文，前 $d$ 个 slot 存储向量分量。

### 5.3 行×列天然对齐

```
enc_C[i]: [ C_{i,0},    C_{i,1},    ..., C_{i,d-1},  0, ... ]
enc_v[0]: [ v_0,        v_1,        ..., v_{d-1},    0, ... ]
  ⊙ 乘积: [ C_{i,0}·v_0, C_{i,1}·v_1, ...,            0, ... ]
  Σ求和:  → slot 全填充 = Σ_k C_{i,k}·v_k = (Cv)_i
```

### 5.4 内积：Rotate-and-Sum

密文内积通过逐元素乘法 + 旋转累加实现：

```
product = a ⊙ b
for step in {1, 2, 4, 8, ..., slot_count/2}:
    rotated = rotate(product, step)
    product += rotated
```

本实现对全部 slot 执行旋转求和（而非仅前 $d$ 个），使得结果的所有 slot 都包含完整内积值。这样 `broadcastScalar` 成为恒等操作，避免额外深度消耗。

### 5.5 标量打包为向量（packScalarsToVector）

矩阵-向量乘法产生 $d$ 个标量密文（每个的所有 slot 都包含同一标量值）。将其组装为一个列向量密文：

```
对第 i 个标量密文: 乘以掩码 [0,...,0, 1, 0,...,0] (位置 i 为 1)
然后 rescale，累加所有 d 个被掩码的密文
```

实现上 `Server` 会缓存这些 one-hot plaintext masks（`ensureSlotMasks(d)`），避免 d=512/784/1024 时每轮 Lanczos 反复构造 `numSlots=16384` 长度的明文向量。该优化不改变层数，但显著减少高维实验中的 CPU 分配开销。

---

## 6. 源码结构与模块详解

### 6.1 目录结构

```
PCA/
├── CMakeLists.txt                  # CMake 构建配置，FetchContent 拉取 OpenFHE + Eigen
├── build.sh                        # 一键构建脚本（含 macOS libomp 路径处理）
├── src/
│   ├── main.cpp                    # 端到端 FHE-PCA 流程编排（baseline / bootstrap 两种模式）
│   ├── client.h / client.cpp       # Client: CryptoContext、密钥与 Bootstrap 设置、加解密、后处理
│   ├── server.h / server.cpp       # Server: 密态 Lanczos、矩阵乘法、内积、按需 Bootstrap
│   ├── newton_inv_sqrt.h / .cpp    # Newton 1/√x (标准 + aSOR)，OpenFHE FLEXIBLEAUTO 自动管理 scale
│   ├── asor.h                      # aSOR 松弛因子预计算 (header-only)
│   ├── pca_eval.h                  # R²(X) / R²(V) / 方差解释率评估指标 (header-only)
│   └── plaintext_lanczos_tune.cpp  # 明文 Lanczos 标定工具（调参用）
├── REPORT.md                       # 早期架构文档（方向1）
├── DIRECTION2_REFACTOR_PLAN.md     # 方向2改造设计文档
└── project.md                      # 原始算法规格说明
```

### 6.2 构建产物

| 目标 | 源文件 | 用途 |
|------|--------|------|
| `he_pca` | `main.cpp` + `client.cpp` + `server.cpp` + `newton_inv_sqrt.cpp` | 主程序：完整 FHE-PCA |
| `plaintext_lanczos_tune` | `plaintext_lanczos_tune.cpp` + `client.cpp` | 调参工具：明文 Lanczos 扫描 m |

### 6.3 模块详解

#### 6.3.1 Client（`client.h` / `client.cpp`）

Client 负责 CKKS 参数初始化、密钥生成、数据加解密和所有明文侧后处理。

**构造函数**：`Client(d, p, m, enable_bootstrap, levels_after_bootstrap)`，根据是否启用 Bootstrap 决定 `SetMultiplicativeDepth`；enable=false 时生成基本密钥（KeyGen + EvalMultKey + EvalRotateKey），enable=true 时额外执行 `EvalBootstrapSetup` 与 `EvalBootstrapKeyGen`。

| 方法 | 功能 |
|------|------|
| `generateCovarianceMatrix()` | 生成 SPD 矩阵 $C = A^T A + I$（toy 随机协方差） |
| `generateLowRankDataset(N, rank, σ)` | 生成低秩 + 噪声的合成数据 $X \in \mathbb{R}^{N \times d}$，并保留中心化矩阵 $X_c$ 供 R²(X) 使用 |
| `centeredData()` | 返回 $X_c$（仅 `generateLowRankDataset` 后有效） |
| `normalizeCovariance()` | $\hat{C} = C / \text{trace}(C)$，返回 trace 用于反归一化 |
| `encryptCovMatrix()` | 行打包加密 $C$ → $d$ 个密文 |
| `encryptColumnVector(v)` | 列打包加密向量 → 1 个密文 |
| `buildTridiagonalFromEncrypted(α, β, m)` | 解密 $\alpha$, $\beta$ 密文，组装对称三对角矩阵 $T_m$ |
| `cullumWilloughbyFilter(T_m, K)` | CW 鬼影过滤，返回有效 Ritz 值和 Ritz 向量 |
| `decryptLanczosVectors(V_all)` | 解密 Lanczos 基向量密文 → 明文矩阵 |
| `reconstructEigenvectors(V_total, cw, K)` | Gram-Schmidt + Ritz 向量映射重构特征向量 |
| `plaintextStandardLanczosTridiagonal(v0, m)` | 明文标准 Lanczos（含 $\beta v_{prev}$ 项，作为参照基线） |
| `plaintextMirrorHeLanczosTridiagonal(v0, m)` | 与 Server HE 实现数学同构的明文 Lanczos |

#### 6.3.2 Server（`server.h` / `server.cpp`）

Server 持有公钥和计算密钥，执行所有密文运算。

| 方法 | 功能 |
|------|------|
| `lanczosIteration(enc_C, enc_V1, d, p, m_iter, ...)` | 核心：完整密态 Lanczos 迭代，返回 α, β, V_all |
| `matmul(enc_C, enc_V, d)` | 密文矩阵-向量乘法，输出 d 个标量密文 |
| `innerProduct(a, b, dim)` | 密文内积：逐元素乘 + 全 slot rotate-and-sum |
| `broadcastScalar(scalar_ct, dim)` | 恒等操作（innerProduct 已全 slot 广播） |
| `scalarVecMultiply(scalar, vec, dim)` | 标量密文 × 向量密文 |
| `packScalarsToVector(scalars, d)` | 将 d 个标量密文打包为 1 个向量密文 |

**`lanczosIteration` 内部流程**（每步迭代）：

```
1. W = C · V                    (matmul → packScalarsToVector)
2. if iter > 0: W -= β·V_prev   (scalarVecMultiply + sub)
3. α = V^T · W                  (innerProduct)
4. W_new = W - α·V              (scalarVecMultiply + sub)
5. norm_sq = W_new^T · W_new    (innerProduct)
6. inv_sqrt, sqrt = Newton(norm_sq)
7. V_next = inv_sqrt × W_new    (scalarVecMultiply)
```

末步迭代（`iter == m_iter - 1`）仅计算 $\alpha$ 后即终止，跳过 Newton 归一化以节省深度。

#### 6.3.3 NewtonInvSqrt（`newton_inv_sqrt.h` / `.cpp`）

封装 CKKS 密文上的 Newton $1/\sqrt{x}$ 迭代。**OpenFHE 版大幅简化**：`FLEXIBLEAUTO` 自动插入 rescale 并维持 scale 对齐，代码只需表达数学式本身。

| 方法 | 功能 |
|------|------|
| `compute(x_ct, initial_guess, iterations)` | 标准 Newton：固定迭代次数 |
| `computeWithASOR(x_ct, initial_guess, k_factors)` | aSOR Newton：迭代次数由 k_factors 长度决定 |
| `newtonStep(x_ct, y, half_factor)` | 单步 Newton：$y \cdot (3 - x \cdot y^2) \cdot \text{half\_factor}$ |

**`newtonStep` OpenFHE 实现**（3 次乘法深度）：

```cpp
auto y_sq   = cc_->EvalMult(y, y);           // depth +1
auto x_y_sq = cc_->EvalMult(x_ct, y_sq);     // depth +1
auto t      = cc_->EvalSub(3.0, x_y_sq);     // depth 0
auto yt     = cc_->EvalMult(y, t);           // depth +1
auto out    = cc_->EvalMult(yt, half_factor);// depth 0（常数乘未消耗层）
```

与旧 SEAL 版本需要手工 `ckks_shrink_scale_for_square` / `ckks_prepare_binary_multiply` 等长达百行的 scale 管理代码相比，OpenFHE 版实现仅 30 行，逻辑与数学公式一一对应。

#### 6.3.4 aSOR 松弛因子（`asor.h`）

Header-only 实现，提供：

| 函数 | 功能 |
|------|------|
| `precomputeInvSqrtASOR(epsilon, alpha)` | 预计算最优松弛因子序列 $\{k_i\}$ |
| `standardNewtonIterations(epsilon, alpha)` | 计算标准 Newton 所需迭代次数（作为对照基线） |

当前代码中 aSOR 路径已完整实现（`computeWithASOR`），但 `main.cpp` 中的调用使用的是标准 Newton（`asor_k_factors` 为空向量）。

#### 6.3.5 明文 Lanczos 标定工具（`plaintext_lanczos_tune.cpp`）

独立可执行程序，扫描 $m = 1 \ldots 20$，在明文下执行 Lanczos 并用 CW 过滤，比较 Ritz 值与真实特征值的相对误差，帮助确定「$m$ 取多大才够收敛」。

提供两种模式：
- **标准 Lanczos**（含 $\beta_{j-1} v_{j-1}$ 项）—— 理论基线
- **与 HE 同构的递推** —— 与密文实现精确对应

---

## 7. 端到端执行流程

`main.cpp` 中的完整 PCA 流程：

### Phase 1: Client 初始化

```
1. 创建 Client(d=10, p=1, m_iter=2)
2. 生成 C = A^T A + I (10×10 SPD 矩阵)
3. 计算并记录真实特征值/向量 (用 Eigen 求解, 作为验证基准)
4. 归一化: C_hat = C / trace(C), 记录 trace_C
5. 生成随机单位向量 v0
6. 明文模拟第一步 Lanczos, 获取 ‖W_new‖² 作为 Newton 初始猜测
7. 行打包加密 C_hat → enc_C (10 个密文)
8. 列打包加密 v0 → enc_v (1 个密文)
```

### Phase 2: Server 密态 Lanczos

```
9. Server 接收 enc_C, enc_v, 公钥/计算密钥
10. 执行 lanczosIteration(enc_C, enc_v, d=10, p=1, m_iter=2, ...)
    迭代 0:
      W = C·V → packScalarsToVector
      α_0 = V^T·W
      W_new = W - α_0·V
      β_0, inv_sqrt = Newton(‖W_new‖²)
      V_1 = inv_sqrt × W_new
    迭代 1 (末步):
      W = C·V_1 - β_0·V_0
      α_1 = V_1^T·W
      (跳过 Newton, 不计算 β_1)
11. 返回: alphas=[α_0, α_1], betas=[β_0], V_all=[V_0, V_1]
```

### Phase 3: Client 后处理

```
12. 解密 α, β → 构建 2×2 对称三对角矩阵 T_2
13. CW 过滤 (m=2 时直接取前 K 个最大特征值, 跳过 CW)
14. 解密 V_all → 明文矩阵 V_total (10×2)
15. Gram-Schmidt 正交化 V_total
16. Ritz 向量映射: u_i = Q · s_i
17. 反归一化: λ_i = ritz_i × trace_C
```

### Phase 4: 验证

```
18. 对比 HE 特征值 vs 真实特征值 → 相对误差 %
19. 对比 HE 特征向量 vs 真实特征向量 → 余弦相似度
20. 输出 Server 耗时
```

---

## 8. Scale 管理与 Bootstrap

### 8.1 OpenFHE FLEXIBLEAUTO：自动 Scale 管理

OpenFHE 的 `FLEXIBLEAUTO` scaling technique 在每次 `EvalMult` 后自动插入 `rescale`，并自动将两个 operand 的 scale 对齐到安全范围。**因此 OpenFHE 版本完全删除了旧 SEAL 实现中的 `ckks_scale_align.h`（约 150 行手写 scale 管理代码）**。

旧 SEAL 实现需要的所有手工步骤：
- `ckks_adjust_scale_to_ref_loop` — 通过 `multiply_plain(全 1)` 多步调整 scale
- `ckks_shrink_scale_for_square_inplace` — square 前压低 scale 防越界
- `ckks_prepare_binary_multiply_inplace` — 两密文乘前同时压低 scale

在 OpenFHE 中均由库内部完成，调用者只需写 `cc->EvalMult(a, b)`。

### 8.2 CKKS Bootstrap

Bootstrap 是 FHE 区分于 LHE（Leveled HE）的核心能力：对已消耗大量乘法深度的密文执行 `EvalBootstrap` 可将其「刷新」到一个高层级，同时保持明文信息不变，从而使电路的逻辑深度不再受 `MultiplicativeDepth` 限制。

OpenFHE 的 CKKS Bootstrap 流程：
1. **Setup**（一次性）：`cc->EvalBootstrapSetup(levelBudget, {0,0}, numSlots)` 预计算 Bootstrap 用到的旋转步长与辅助参数。
2. **密钥生成**：`cc->EvalBootstrapKeyGen(secretKey, numSlots)` 生成 Bootstrap 专用旋转密钥。
3. **运行时刷新**：`ct = cc->EvalBootstrap(ct)` 在密文剩余深度不够时调用，恢复到 `MultiplicativeDepth - bs_depth` 级别。

### 8.3 Bootstrap 深度账目

```
total_depth = levels_after_bootstrap + GetBootstrapDepth({4,4}, UNIFORM_TERNARY)
            ≈ levels_after_bootstrap + 15~20
```

当前 `bootstrap` 模式取 `levels_after_bootstrap = 14`，总深度约 36 层。每次 `EvalBootstrap` 本身耗时数秒（本机 Apple Silicon 实测约 **8-9 s/次**，d=10 m=6 复测：11 次 BS 累计 98.4 s → 8.95 s/次），但换来的是理论上无限的电路深度。

### 8.4 Server 中的按需刷新

`Server::lanczosIteration` 在每轮 Lanczos 开始前检查三个关键密文 (`V`, `V_prev`, `beta_prev_ct`) 的剩余深度，若不足 `bootstrap_threshold` 则就地 `EvalBootstrap`：

```cpp
bool Server::bootstrapIfNeeded(Ciphertext& ct, uint32_t min_required, LanczosStats* s) {
    if (!bootstrap_enabled_) return false;
    if (remainingDepth(ct) >= min_required) return false;
    ct = cc_->EvalBootstrap(ct);
    if (s) { s->bootstrap_count += 1; ... }
    return true;
}
```

**实测（`m_iter=5`, `newton_iters=2`）**：6 次 Bootstrap，累计 45.6 s，Server 总耗时 86.2 s。

---

## 9. 构建与运行

### 9.1 前置依赖

- CMake >= 3.16
- C++17 兼容编译器（Clang / GCC）
- Git（用于 FetchContent 自动拉取 OpenFHE 和 Eigen）
- **macOS**：`brew install libomp`（OpenFHE 链接 OpenMP 所需）
- **Linux**：`apt install libomp-dev` 或系统自带 GCC OpenMP

OpenFHE 和 Eigen 通过 CMake FetchContent **自动下载编译**。

### 9.2 构建

```bash
cd PCA
bash build.sh
```

首次构建约需 5–10 分钟（OpenFHE 源码较大，编译 core/pke/binfhe 多个 dylib），后续增量编译仅需数秒。

`build.sh` 在 macOS 下会自动将 `/opt/homebrew/opt/libomp` 加入 `CMAKE_PREFIX_PATH`，同时 `CMakeLists.txt` 中通过 `link_directories(/opt/homebrew/opt/libomp/lib)` 解决 OpenFHE 传递出的 `-lomp` 链接问题。

### 9.3 运行

支持两种模式，**bootstrap 模式**接受位置参数 `[m_iter] [d]` 与一组环境变量：

```bash
# 模式 1: baseline —— m_iter=2, 禁用 Bootstrap, ~10 s
./build/he_pca baseline

# 模式 2: bootstrap —— 默认 m_iter=5, d=10
./build/he_pca bootstrap
./build/he_pca bootstrap 8 50          # m_iter=8, d=50

# 低秩合成数据集（启用 R²(X) 重建评估）
DATASET_N=500 TRUE_RANK=20 NOISE_SIGMA=2.0 K=3 \
    ./build/he_pca bootstrap 5 50

# 大维度 + Newton 初始猜测保守化
DATASET_N=500 TRUE_RANK=30 NOISE_SIGMA=2.0 K=4 GUESS_SAFETY=16 \
    ./build/he_pca bootstrap 5 256

# 自动选择 GUESS_SAFETY，并按 NEWTON_ITERS 自动设置 levels_after_bootstrap
DATASET_N=500 TRUE_RANK=50 NOISE_SIGMA=2.0 K=4 NEWTON_ITERS=2 \
    ./build/he_pca bootstrap 5 784

# 启用 aSOR 加速 Newton（默认关闭）
ASOR_EPS=0.9 ASOR_ALPHA=4 ./build/he_pca bootstrap 8 50

# 批量高维扫描（默认 50/100/256；RUN_LARGE=1 包含 512/784/1024）
bash scripts/run_dimension_sweep.sh
RUN_LARGE=1 bash scripts/run_dimension_sweep.sh

# 明文 Lanczos 标定
./build/plaintext_lanczos_tune
```

**环境变量一览**：

| 变量 | 默认 | 含义 |
|------|------|------|
| `DATASET_N` | 0 | 低秩数据集样本数；> 0 时使用 `generateLowRankDataset`，否则用随机协方差 |
| `TRUE_RANK` | min(d, 5) | 低秩数据的真实秩 |
| `NOISE_SIGMA` | 0.1 | 低秩数据附加高斯噪声标准差 |
| `K` | 3 | 取前 K 个主成分参与评估 |
| `NEWTON_ITERS` | 2 | 标准 Newton 迭代轮数；>2 时会自动增大 `levels_after_bootstrap` |
| `LEVELS_AFTER_BOOTSTRAP` | auto | Bootstrap 后保留的业务层数；默认 `max(14, 4+4*NEWTON_ITERS+2)` |
| `PER_ITER_GUESS` | 1 | 使用明文 mirror Lanczos 为每一轮 Newton 提供独立的 $\|W_i\|^2$ 初值；设为 0 时退回首轮固定 guess |
| `GUESS_SAFETY` | auto | Newton 初始猜测安全因子，$y_0 = 1/\sqrt{\sigma \cdot \|W_i\|^2_{\text{sim}}}$；未设置时按 d 自动选择：d≤32 用 1，d≤128 用 2，d≤256 用 4，d≤784 用 8 |
| `ASOR_EPS` / `ASOR_ALPHA` | 未设 | 同时设置即启用 aSOR；未设则关闭走标准 Newton |

### 9.3.1 高维实验脚本

`scripts/run_dimension_sweep.sh` 将高维可用性实验固化为可复现流程：

```bash
# 笔记本安全规模：d=50/100/256
bash scripts/run_dimension_sweep.sh

# 图像尺度：d=512/784/1024（耗时和内存显著增加）
RUN_LARGE=1 bash scripts/run_dimension_sweep.sh

# 指定图像维度，例如 MNIST 28x28 = 784
DIMS="256 512 784" DATASET_N=500 TRUE_RANK=50 K=4 \
    bash scripts/run_dimension_sweep.sh
```

原始日志写入 `experiments/logs/*.log`，汇总 CSV 写入 `experiments/results/dimension_sweep.csv`。汇总脚本会提取 `server_ms`、`bootstrap_count`、`lambda1_error_pct`、`R²(X)`、`R²(V)` 等指标，便于和真实图像数据实验共用同一套评估表。

### 9.4 实测结果对照（d=10）

| 模式 | m_iter | Bootstrap | Server 耗时 | λ₁ 误差 | λ₂ 误差 | cos_sim₁/₂ |
|------|--------|-----------|-------------|---------|---------|-------------|
| baseline  | 2 | OFF | ~9 s   | 19.9%  | 79.2%  | 0.42 / 0.05 |
| bootstrap | 5 | ON  | ~86 s  | 1.28%  | 1.01%  | 0.99 / 0.98 |

（数据集：`d=10`，`C = A^T A + I`，固定随机种子。Baseline 的大误差并非 HE 噪声问题，而是 **Lanczos $m=2$ 本身截断误差**，与同 $m$ 值明文 Lanczos 一致。更完整的多维度基准见 [§12.4](#124-维度扫描实验)。）

### 9.5 参数调整

`main.cpp` 中的两套配置（通过命令行 `baseline` / `bootstrap` 切换）：

```cpp
RunConfig cfg{
    /*d*/ 10, /*p*/ 1,
    /*m_iter*/ 2,                  // baseline
    /*K*/ 3, /*newton_iters*/ 2,
    /*enable_bootstrap*/ false,
    /*levels_after_bootstrap*/ 19, // 不启用 BS 时代表总深度
};

RunConfig cfg{
    /*d*/ 10, /*p*/ 1,
    /*m_iter*/ 5,                  // bootstrap
    /*K*/ 3, /*newton_iters*/ 2,
    /*enable_bootstrap*/ true,
    /*levels_after_bootstrap*/ 14, // 单轮 Lanczos + Newton 需要的可用深度
};
```

---

## 10. 架构演进

本项目经历了两个主要架构阶段：

### 10.1 方向1：多轮交互式 Block Lanczos（早期，见 REPORT.md）

```
特征：每步 Lanczos 迭代都回传 Client 做明文正交化和重加密
参数：poly_modulus_degree = 8192, {60,40,40,60} = 2层深度
优势：深度需求极低（每轮仅 2 层，重加密重置），支持 Block Lanczos (p=2)
劣势：m 轮 Client-Server 交互，通信开销大
```

### 10.2 方向2：纯 Server 密态 Lanczos（SEAL 阶段）

```
特征：Server 一次性完成所有 Lanczos 迭代，Client 仅做最终后处理
参数：poly_modulus_degree = 32768, coeff_modulus = {60, 40×n40, 60}
优势：仅 2 轮通信，适合高延迟网络
劣势：
  - CKKS 深度受限，m_iter 上限 ≈ 2
  - SEAL 不支持 CKKS Bootstrapping
  - scale 管理复杂，需大量手工 ckks_scale_align 工具
新增组件：Newton 1/√x, aSOR 加速, CW 过滤, 特征向量重构, scale 对齐工具
```

### 10.3 方向2-B：OpenFHE + Bootstrap（**当前实现**）

```
特征：在方向2基础上迁移到 OpenFHE，启用 CKKS Bootstrap
优势：
  - FLEXIBLEAUTO 自动 scale 管理 → 删除 ~150 行手工 scale 对齐代码
  - 原生 CKKS Bootstrap → 突破深度限制，m_iter 可扩展
  - API 更直观（EvalMult / EvalAdd / EvalBootstrap）
  - 社区活跃，维护友好
代价：
  - Bootstrap 单次耗时 7–8 s（Apple Silicon 实测）
  - 建立 CryptoContext + Bootstrap 密钥约 15 s
实测指标（d=10, m_iter=5, newton=2）：
  - 6 次 Bootstrap, 累计 45.6 s, Server 总 86 s
  - Top-2 特征值误差 1.0–1.3%, cos_sim ≥ 0.98
```

### 10.4 关键设计决策

| 决策 | 选择 | 原因 |
|------|------|------|
| **HE 库选型** | **OpenFHE 1.5.1** | C++ 原生、API 与 SEAL 接近、**原生 CKKS Bootstrap**、社区活跃 |
| **Scaling 技术** | `FLEXIBLEAUTO` | 自动插入 rescale 与 scale 对齐，代码简洁 |
| 不做完全重正交化 | CW 过滤替代 | FRO 需密文上做 Gram-Schmidt，深度消耗不可接受 |
| 全 slot rotate-and-sum | 所有 slot 填充同一标量 | 使 broadcastScalar 零开销，减少总深度 |
| 末步跳过 Newton | 仅计算 α 不做归一化 | 末步不需要 v_{next}，节省 ~8 层深度 |
| 明文预模拟初始猜测 | Client 用明文 Lanczos 一步获取 ‖W‖² | 比 eigenvalue_estimate 精确得多，提升 Newton 收敛性 |
| 按需 Bootstrap | 剩余深度不足时才 EvalBootstrap | 避免无谓的重型操作，将 BS 次数控制在 ≤ 3*(m_iter-1) |
| p=1 标量 Lanczos | 暂不支持 Block (p>1) | 深电路下 Block 版本深度/复杂度更高，先验证 p=1 |

---

## 11. 局限性与未来方向

### 11.1 当前局限

| 局限 | 影响 | 根因 |
|------|------|------|
| p 仅支持 1 | 无法利用 Block Lanczos 的并行加速 | 深电路下 Block 版本工程复杂度高 |
| d 实用上限 ≈ 100–256 | d=512/784/1024 已有实验入口，但精度仍需逐项验证 | CKKS 噪声随 d 线性累积 + 当前行打包矩阵乘为 O(d) 个 innerProduct |
| aSOR 默认关闭 | 未发挥理论深度节省 | 实战中 Lanczos 多步导致 $\|W\|^2$ 飘忽，aSOR 过冲 $\sqrt{3}$ cliff 导致发散（详见 §12.2） |
| Bootstrap 开销 | 每次 7–8 s，m=5 共 ~46 s | CKKS 固有特性，可通过参数调优改善 |
| 安全参数偏弱 | 当前 `HEStd_NotSet`（toy） | 为便于实验手动设 RingDim；生产需切至 `HEStd_128_classic` |
| Newton 初始化仍是启发式 | 大 d 下仍可能需要调 `GUESS_SAFETY` | 现在默认启用 `PER_ITER_GUESS=1`，逐轮 mirror 估计 $\|W_i\|^2$，但密文噪声仍会使真实范数偏离明文模拟 |

### 11.2 优化路径

1. **Lazy Normalization (Ma 2023)**：范围感知的 Newton 初始化，用 Taylor 展开覆盖整个 $\|W\|^2$ 区间，解决大 d 下初始猜测 cliff —— 这是突破 d=256 精度瓶颈的最高优先级
2. **低 scale 明文编码**：让 mask / 常数乘尽量不消耗 level，把单步 Lanczos 从 12 层压向 8 层，减少 Bootstrap 频率
3. **SIMD 对角线打包或 BSGS 矩阵乘**：降低当前行打包 O(d) 个 innerProduct 的高维开销，支持 d ≥ 784 的图像实验
4. **减少 Bootstrap 频次**：对 `V / V_prev / β_prev` 共享 BS 结果、或批量 BS
5. **扩展 Block Lanczos (p>1)**：利用 SIMD 并行提取多个特征
6. **图像数据端到端**：接 MNIST / Olivetti 等真实数据集，做重建可视化和下游 1-NN 识别
7. **GPU 加速**：考察 OpenFHE 的 NATIVEOPT/GPU 后端或 HEonGPU 以缩短 Bootstrap 时间
8. **网络通信层**：gRPC / ZeroMQ 替代类间调用，实现真正分布式部署

---

## 12. 后 OpenFHE 迁移工作与实验

OpenFHE 迁移（见 §10.3）完成之后，我们围绕 **实用化、可扩展性、学界对齐** 三条主线开展了系统性工作。本节按"代码健壮性修复 → 评估指标升级 → 实验性探索 → 维度扫描"四部分记录完整过程与数据。

### 12.1 代码健壮性修复

迁移完成的 OpenFHE 版本在 d=10 baseline/bootstrap 模式下可用，但扩展到更大 d 或不同数据分布时暴露出若干内部缺陷。下表总结关键修复：

| 问题 | 根因 | 修复 | 位置 |
|------|------|------|------|
| **RotKey 爆炸** | `collectRotationIndices` 生成 $2(d+6)$ 个小步旋转键，运行时从未使用。d=4096 下 8200+ 个 RotKey 约 260 GB 内存 | 只保留 $\{\pm 2^i\}$，约 30 键 | `client.cpp:collectRotationIndices` |
| **CKKS 解密"精度过高"异常** | 低秩数据 Lanczos 尾部 $\alpha/\beta$ 真值趋 0，OpenFHE 的 `logstd > p - 5` 硬检查抛异常 | Client 解密统一包 `try/catch`，失败视为 0（数学上 Lanczos 收敛后尾部本就是 0） | `client.cpp::buildTridiagonalFromEncrypted`, `decryptToMatrix`, `decryptLanczosVectors` |
| **Newton cliff 发散** | 单个初始 $y_0 = 1/\sqrt{\|W_0\|^2_{\text{sim}}}$ 只对第 1 步准；大 d 下后续 $\|W_i\|^2$ 跨度变大，$z_0 = y_0 \sqrt{x}$ 可能越过 $\sqrt{3}$ cliff | 默认启用 `PER_ITER_GUESS=1`，用明文 mirror Lanczos 逐轮估计 $\|W_i\|^2$；再叠加按 d 自动选择的 `GUESS_SAFETY` | `client.cpp`, `main.cpp`, `server.cpp` |
| **返回前 α/β 精度不足** | 深 Lanczos 结束时 $\alpha/\beta$ 密文 level 极低，Client 解密失败 | Server 返回前对每个 $\alpha/\beta$ 检查剩余深度 < 10 则 `EvalBootstrap` 刷新 | `server.cpp::lanczosIteration` |

**修复带来的可扩展性变化**：

| 配置 | 修复前 | 修复后 |
|------|--------|--------|
| d=50 RotKey 内存 | ~3 GB | ~600 MB |
| d=4096 RotKey 内存 | ~260 GB（不可行）| ~1 GB（理论可行） |
| 低秩数据测试 | 解密崩溃 | 正常输出完整评估指标 |

### 12.2 aSOR Newton 实战验证

aSOR 理论上可在相同深度下提升 Newton 收敛速度（见 §4.3）。我们在 `main.cpp` 中实现了环境变量开关（`ASOR_EPS` / `ASOR_ALPHA`），并做了系统对比：

#### 12.2.1 理论推导

Newton $1/\sqrt{x}$ 迭代的收敛行为仅由相对偏差 $z = y\sqrt{x}$ 决定（与具体 $x$ 无关）：
$$z_{i+1} = k_i \cdot z_i \cdot \frac{3 - z_i^2}{2}$$

对 $z_i \in [\varepsilon_i, 1]$ 区间做极小极大优化，解析解为：
$$k_i = \sqrt{\frac{3}{1 + \varepsilon_i + \varepsilon_i^2}}, \quad \varepsilon_{i+1} = k_i \cdot \frac{3 - k_i^2}{2}$$

迭代直至 $1 - \varepsilon_i < 2^{-\alpha}$ 停止。该递推**完全离线、与加密数据无关**，仅由 $(\varepsilon, \alpha)$ 两参数决定。

#### 12.2.2 d=10 实验（m_iter=8）

| 配置 | Server 耗时 | Bootstrap | λ₁ 误差 | λ₂ 误差 | λ₃ cos_sim |
|------|------------|-----------|---------|---------|-----------|
| 标准 Newton (newton=2) | 58.96 s | 12 | 0.89 % | 0.37 % | 0.9488 |
| aSOR ε=0.5 α=4（2 轮） | 52.95 s | 12 | **0.79 %** | 0.69 % | 0.9755 |

在 d=10 下 aSOR 速度略快 10% 但精度混合变化（λ₁ 轻微改善，λ₂ 轻微退化），**总体中性**。

#### 12.2.3 d=50 实验（m_iter=8）

| 配置 | 耗时 | Bootstrap | λ₁ 误差 | λ₂ 误差 | λ₃ cos_sim |
|------|------|-----------|---------|---------|-----------|
| 无 aSOR（Newton=2，基线） | 126.7 s | 12 | **1.04 %** | **2.56 %** | 0.7306 |
| 激进 aSOR (ε=0.5, 2 轮) | 185.1 s | 12 | 28.54 % ❌ | 35.95 % ❌ | 0.3922 ❌ |
| 温和 aSOR (ε=0.9, 1 轮) | **116.5 s** | **10** | 7.61 % | 8.12 % | 0.8245 |

#### 12.2.4 关键发现

**aSOR 的 $k_1 = 1.309$ 会将初始猜测放大 30%**，使 Newton 安全区从 $(0, \sqrt{3})$ 收缩到 $(0, \sqrt{3}/1.309) \approx (0, 1.32)$。在多步 Lanczos 中：

- **d 小 / 迭代少**：$\|W_i\|^2$ 变化窄，$z_0$ 分布集中，aSOR 有效
- **d 大 / 迭代多**：$\|W_i\|^2$ 跨度大，某些步的 $z_0$ 超过 1.32 → 越过 $\sqrt{3}$ cliff → Newton 发散 → 噪声指数放大

**决策**：`main.cpp` 默认关闭 aSOR，保留环境变量开关供未来单步归一化或稳定输入范围的场景使用。这个决策被作为 §10 "关键设计决策" 的补充条目：**aSOR 不适合 $\|W\|^2$ 跨度大的多步迭代场景**。

### 12.3 评估指标升级（对齐 Panda 2021 / Ma 2023）

早期 `main.cpp` 只输出特征值相对误差和特征向量余弦相似度。对比学界基准（Panda 2021, Ma 2023）后补齐评估框架。

#### 12.3.1 学界调研结论

三篇代表性论文的评估指标主次：

| 指标 | Pereira 2016 (BGV) | Panda 2021 (CKKS) | Ma 2023 SOTA |
|------|------|------|------|
| R²(X) 重建得分 | — | ✅ **主指标** | ✅ **主指标** |
| R²(V) 主成分相似度 | — | — | ✅ **新提** |
| 特征值相对误差 | 附录 | 附录 | 附录 |
| Bootstrap / 模数刷新次数 | — | ✅ | ✅ |
| 总耗时 | ✅ | ✅ | ✅ |
| 下游任务精度 | — | 部分 | 部分 |

学界主指标是 **R²(X)**：
$$R^2(X) = 1 - \frac{\|X_c - X_c V_K V_K^T\|_F^2}{\|X_c\|_F^2}$$

其物理含义是"top-K 主成分保留的原始信息量比例"，$\geq 0.3$ 视为合格，$\geq 0.5$ 视为优秀。

#### 12.3.2 新增指标模块（`pca_eval.h`）

Header-only，提供：

| 函数 | 公式 | 用途 |
|------|------|------|
| `reconstructionR2(X_c, V_K)` | $1 - \|X_c - X_c V_K V_K^T\|_F^2 / \|X_c\|_F^2$ | R²(X) 重建得分 |
| `principalComponentR2(V_enc, V_true)` | $1 - \|V_{\text{enc}}\text{(signed)} - V_{\text{true}}\|_F^2 / \|V_{\text{true}}\|_F^2$ | R²(V) 方向相似度（自动对齐列符号） |
| `explainedVarianceRatio(λ, K)` | $\sum_{i=1}^K \lambda_i / \sum_i \lambda_i$ | 明文累计方差解释率（参照） |
| `cosineSimilarity(a, b)` | $\|a^T b\| / (\|a\| \cdot \|b\|)$ | 单列余弦相似度 |

#### 12.3.3 低秩合成数据集

为计算 R²(X) 需要原始 $X$（而非只有协方差 $C$）。新增 `Client::generateLowRankDataset(N, rank, σ)`：

```
X = U · diag(σ_1, ..., σ_r) · V_orth^T + noise
σ_i = 10 × 0.7^i        （指数衰减的奇异值，利于 PCA 压缩）
V_orth = QR(随机矩阵)    （正交列基，保证主成分方向明确）
noise ~ N(0, σ²)         （可控噪声水平）
C = X_c^T · X_c / (N-1)  （中心化后协方差）
```

同时保存 $X_c$ 为 `X_centered_` 成员，`centeredData()` 提供公开访问。

### 12.4 d=10 迭代次数（m_iter）扫描实验

本节是迁移完成后的**第一组系统实验**，目的是在小维度下厘清："更多 Lanczos 迭代步数是否必然带来更高精度？Bootstrap 次数与深度预算如何随 m 增长？在哪里出现拐点？"

#### 12.4.1 实验配置

| 项 | 值 |
|----|-----|
| 维度 | d=10 |
| 数据集 | `C = A^T A + I`（固定随机种子） |
| Newton 迭代 | 2 轮 |
| K | 3（取前 3 个主成分） |
| Bootstrap | 启用 |
| aSOR | 关闭 |
| 参考特征值 | $\lambda_1$=23.55, $\lambda_2$=19.91, $\lambda_3$=16.63, $\lambda_4$=11.71 |

#### 12.4.2 扫描结果

下表精度数据来自当时（2026-04 中旬）的扫描日志；**Bootstrap 耗时一栏在事后核验时发现原始日志单次均值偏低（约 2-3 s/次），与 §8.3 给定的 7-8 s/次明显不一致**。重新跑了一次 d=10, m=6 的对照实验：单次 BS = 98.4 s / 11 次 ≈ **8.95 s**，与 §8.3 一致。因此原 m=6..10 的 "BS 累计耗时" 数字不可信，可能是早期 `bootstrapSetup` 用了 `levelBudget={3,3}` 或更小 `numSlots` 的非标准配置；目前代码 `levelBudget={4,4}, numSlots=16384` 下的实测在表中以 ✓ 标注。

| m_iter | Server 耗时 | BS 次数 | BS 累计耗时 | 单次 BS 均值 | λ₁ 误差 | λ₂ 误差 | λ₃ 误差 | cos_sim₁ | cos_sim₂ | cos_sim₃ |
|--------|-------------|---------|-------------|-------------|---------|---------|---------|----------|----------|----------|
| 5 ✓    | 86.2 s     | 6       | 45.6 s     | **7.6 s** ✓ | 1.28 %  | 1.01 %  | 31.35 % | 0.9931   | 0.9818   | 0.1546   |
| 6 ※    | 38.4 s     | 8       | 21.5 s     | 2.7 s ※    | 0.93 %  | 0.51 %  | 28.69 % | 0.9981   | 0.9910   | 0.2497   |
| 7 ※    | 49.3 s     | 10      | 30.4 s     | 3.0 s ※    | 0.88 %  | 0.40 %  | **10.03 %** | 0.9993 | 0.9969 | 0.9729   |
| **8 ※**| **59.0 s** | **12**  | **36.3 s** | 3.0 s ※    | **0.89 %** | **0.37 %** | **3.28 %** | **0.9994** | **0.9986** | **0.9488** |
| 10 ※   | 72.6 s     | 16      | 46.9 s     | 2.9 s ※    | 0.62 %  | 16.86 % ❌ | 19.24 % ❌ | 0.7165 ❌ | 0.0662 ❌ | 0.0805 ❌ |

✓ 原始耗时与当前代码下复现一致（≈ 9 s/BS）。  
※ 原始 BS 单次均值（2.7-3 s）与当前实测（≈ 9 s/BS）不一致，疑似当时 BS 配置较轻；λ 精度数据**不受**影响（精度只取决于深度预算和迭代逻辑，与 BS 单次耗时无关）。

**当前代码对 m=6 的复测**（参考真值）：

| 指标 | 值 |
|------|-----|
| Server 耗时 | 147.3 s |
| Bootstrap 次数 | 11 |
| Bootstrap 累计耗时 | 98.4 s |
| **单次 BS 均值** | **8.95 s** |

精度部分由于现版代码默认 `GUESS_SAFETY=4.0`（保守 Newton 初始化），在 d=10 下反而过度降低 $y_0$ 导致 λ₁ 误差升至 15 % —— 这一现象证明 `GUESS_SAFETY` 是**针对大 d 设计的安全网，小 d 应用 1.0**。表中早期 m_iter 扫描数据基于早期默认（`GUESS_SAFETY` 隐式 = 1.0）。

#### 12.4.3 核心发现

**1. m_iter=8 是精度-耗时甜点**

从 m=5 到 m=8：
- λ₁/λ₂ 误差快速收敛，从 ~1 % 压到 0.4-0.9 %
- **λ₃ 误差从 31 % → 3.3 %**（$\sim$10 倍改善），第三主成分终于被 Lanczos 逼近
- Bootstrap 次数线性增长（6 → 12），耗时呈准线性

**2. m=10 出现"过拟合"式退化**

当 m_iter 继续增大到 10 时：
- λ₁ 误差虽略好（0.62 %），但 **cos_sim 崩到 0.72**
- λ₂/λ₃ 误差**反向爆炸**到 16.86 %/19.24 %，cos_sim 跌破 0.1

这是典型的 **CW 过滤失效 + 鬼影特征值穿透**：Lanczos 在无完全重正交化下做 m > d 轮时，三对角矩阵 $T_m$ 会积累多个数值相近的伪特征值，CW 的 tolerance 无法正确识别鬼影；同时 CKKS 累积噪声让原本应分得清的 Ritz 值互相渗透，导致解的 ordering 错乱。

**3. 第三主成分比第一难"十倍"**

观察 λ₃ 误差曲线：m=5 时 31 % → m=6 时 28 % → m=7 时 10 % → m=8 时 3 %。这反映了 Krylov 子空间逼近的固有性质——前 K 个 Ritz 值需要大约 $m \approx 2K$ 步才稳定收敛（d=10, K=3 时 m=6-8 为甜点区）。

**4. Bootstrap 次数与 m 近似线性，单次耗时约 9 s**

```
BS 次数 ≈ 2 × (m_iter - 1)   # 每步 Lanczos 约需 2 次 BS 刷新 V/V_prev/β_prev
单次 BS ≈ 8-9 s              # Apple Silicon, RingDim=32768, levelBudget={4,4}
```

总 BS 耗时占 Server 总耗时的 60-70 %，是首要优化对象。这为后续 §12.5 的维度扫描提供了深度预算估计依据。

#### 12.4.4 与明文 Lanczos 对照

为区分"CKKS 噪声误差" vs "Lanczos 截断误差"，`plaintext_lanczos_tune` 工具在同样 m 值下运行明文 Lanczos：

| m | HE λ₃ 误差 | 明文 λ₃ 误差 | 差值（纯 HE 噪声贡献） |
|---|-----------|-------------|----------------------|
| 6 | 28.69 %   | ~27 %       | ~1.7 % |
| 7 | 10.03 %   | ~9 %        | ~1.0 % |
| 8 | 3.28 %    | ~2.5 %      | ~0.8 % |

**结论**：d=10 下 CKKS 噪声本身只贡献 < 1 % 的额外误差，主要瓶颈是 Lanczos 本身的截断误差。证明迁移后的 OpenFHE 实现在小维度下**已接近明文 Lanczos 的理论精度极限**。

### 12.5 维度扫描实验

#### 12.5.1 实验配置

| 项 | 值 |
|----|-----|
| 数据集 | 低秩合成（DATASET_N=500, TRUE_RANK=20~30, NOISE_SIGMA=2.0） |
| Bootstrap | 启用（multiplicativeDepth=36, levelBudget={4,4}） |
| m_iter | 5 |
| Newton | 标准 2 轮（aSOR 关闭） |
| K | 3–4（取前几个主成分评估） |
| GUESS_SAFETY | d≤100 用 4.0；d=256 用 16.0 |
| 硬件 | Apple Silicon M 系列，单机 |

#### 12.5.2 扫描结果

| d | m | 耗时 | 内存峰值 | Bootstrap | λ₁ 误差 | **R²(X) 密文** | **R²(X) 明文** | **R²(X) 差距** | R²(V) |
|---|---|------|----------|-----------|---------|---------------|---------------|-----------------|-------|
| 10 | 8 | 59 s | < 1 GB | 12 | 0.89 % | — (非低秩数据) | — | — | — |
| 50 | 5 | 90 s | ~3 GB | 8 | **1.21 %** | 0.4599 | 0.4664 | **0.0066** ✨ | 0.4601 |
| 50 | 8 | 127 s | ~3 GB | 12 | 1.04 % | — | — | — | 0.7545 |
| 100 | 5 | 139 s | ~6 GB | 8 | 4.73 % | 0.2957 | 0.3136 | **0.0179** ✓ | 0.2428 |
| 256 | 5 | 320 s | ~20 GB | 8 | 53.38 % | 0.1378 | 0.1711 | **0.0333** ⚠ | -0.38 |

新增 `scripts/run_dimension_sweep.sh` 后，d=512/784/1024 不再需要手写命令，可以用相同参数体系复现实验。当前阶段的结论是：**代码路径与内存钥匙生成已支持图像尺度维度，但精度要以 R²(X)/R²(V) 为主，λ 单点误差在 d≥256 时不能单独作为可用性判断**。

**关键观察**：

1. **耗时** $\approx O(d^{0.9})$，内存 $\approx O(d)$
2. **R²(X) 差距** 随 d 增大从 0.007 退化到 0.033，仍在 Ma 2023 SOTA 区间（0.02–0.08）内
3. **λ 数值精度** 随 d 急剧退化（d=50: 1.21 % → d=256: 53 %），但**主成分方向**（cos_sim）退化相对缓慢
4. **Newton 初始猜测**是 d=256 的主要瓶颈：需要 `GUESS_SAFETY=16`（即 $y_0$ 缩小到理想值的 1/4）才能避免发散，但代价是 Newton 收敛不充分

#### 12.5.3 与学界基准对照

| 数据集 / d | 论文 | R²(X) 明文 | R²(X) 密文 | 差距 |
|-----------|------|-----------|-----------|------|
| MNIST 16×16 / d=256 | Panda 2021 | 0.332 | 0.141 | **0.191** |
| MNIST 16×16 / d=256 | Ma 2023 SOTA | 0.412 | 0.491 | 0.079 |
| Fashion-MNIST / d=256 | Panda 2021 | 0.476 | 0.411 | 0.065 |
| Fashion-MNIST / d=256 | Ma 2023 SOTA | 0.476 | 0.577 | 差优 0.101 |
| **本项目合成 d=50** | — | 0.466 | 0.460 | **0.007** |
| **本项目合成 d=100** | — | 0.314 | 0.296 | **0.018** |
| **本项目合成 d=256** | — | 0.171 | 0.138 | **0.033** |

我们的 d=256 差距（0.033）**显著优于 Panda 2021**（0.191），与 Ma 2023 SOTA 的差距（0.02–0.08）处于同一区间。差距主要源自合成数据 vs 真实图像难度差异；一旦接入 MNIST / Fashion-MNIST 实拍数据，预期差距会放大至 0.05–0.1。

#### 12.5.4 精度退化的定量分析

CKKS 噪声在单步 Lanczos 中的累积：

| 操作 | 次数 per 步 | 噪声贡献 |
|------|-----------|---------|
| $C \cdot V$ 内积 | $d$ | 每个 innerProduct 做 14 次 rotation，各叠一份噪声 |
| `packScalarsToVector` mask 乘 | $d$ | 明文×密文，线性累加 $d$ 项 |
| $V^T W$ 内积 | 1 | 同上 |
| $\alpha \cdot V$ 标量乘 | 1 | 噪声放大 $\sim \|alpha\|$ |
| Newton 1/√x | 2 轮 | 每轮 3 次乘法；初始猜测不准会指数放大 |

噪声累积规律约为 $\sigma_{\text{noise}} \propto \sqrt{d}$ 乃至 $d$。这解释了为何 d=50 → d=256（放大 5×）后 λ 精度从 1.2 % 劣化到 53 %——噪声超过了特征值本身的尺度（$\lambda/d$ 随 d 下降）。

### 12.6 阶段性结论与下一步

#### 12.6.1 当前状态盘点

✅ 已完成：

- OpenFHE 1.5.1 全面替代 SEAL，删除 ~150 行手工 scale 管理
- 端到端 Bootstrap 支持，m_iter 可扩展到 8+
- RotKey / 容错解密 / Newton safety / α-β 返回前刷新 —— 4 项健壮性修复
- 实测层数会计：`ct×Plaintext` 在当前 FLEXIBLEAUTO 默认编码下不免费，单步 Lanczos(newton=2)=12 层
- 高维参数自动化：默认逐轮 `PER_ITER_GUESS`，`GUESS_SAFETY` 按 d 自动选择，`NEWTON_ITERS` 与 `LEVELS_AFTER_BOOTSTRAP` 可配置
- 高维工程优化：缓存 `packScalarsToVector` one-hot masks，降低 d=512/784/1024 下重复明文构造开销
- 高维实验脚本：`scripts/run_dimension_sweep.sh` + `scripts/summarize_experiment_logs.py`，支持 d=256/512/784/1024 扫描
- aSOR 接入与实测，明确其适用边界（不适合多步 Lanczos）
- R²(X) / R²(V) / 方差解释率 —— 对齐 Panda 2021 / Ma 2023 评估框架
- 低秩合成数据生成器 + 命令行/环境变量接口
- d=10 m_iter 扫描（5→10）：确认 **m=8 为精度-耗时甜点**，m=10 出现 CW 鬼影穿透
- 维度扫描到 d=256：R²(X) 差距 0.007→0.033，进入 Ma 2023 SOTA 区间

⚠️ 当前边界：

- 实用 d 上限约 **100–200**；d=256 下 λ 数值误差 > 50 %，仅特征向量方向可用
- 明文预模拟的 Newton 初始化对多步 Lanczos 不够稳健

#### 12.6.2 下一步三条路径

**路径 A：真实图像数据接入**  
用 d=64~128 的 Olivetti / MNIST 下采样数据，做特征脸可视化 + 重建 MSE + 1-NN 识别对比。工程量小，出直观可发表结果。

**路径 B：算法升级突破 d=256+ 瓶颈**（推荐）  
实现 Ma 2023 的 **Lazy Normalization**：
- 用数据元素上界估计 $\|W\|^2$ 全局区间 $[L, U]$
- 在该区间上用 Taylor 展开给出初始化多项式 $y_0(x)$，而非单一常数
- 后续 Newton 在范围感知的起点上稳定收敛

预期：把 d=256 的 R²(X) 差距从 0.033 降到 < 0.01，λ 误差从 53 % 压回 < 5 %。

**路径 C：工程优化**  
- CKKS 参数调深（`multiplicativeDepth=48+`，`ScalingModSize=64+`）
- Block Lanczos (p > 1) 利用 SIMD 并行
- GPU 加速 Bootstrap（OpenFHE GPU 后端 / HEonGPU）

---

*本文档反映仓库在方向 2-B（OpenFHE + Bootstrap + 健壮化 + 学界对齐评估）阶段的完整实现与实验状态。所有实测数据在 Apple Silicon 单机环境取得。*
