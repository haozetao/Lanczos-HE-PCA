# 基于云边协同的 Block Lanczos HE-PCA 原型验证 (PoC)

## 1. 项目概述

本项目实现了一个**基于同态加密（Homomorphic Encryption）的隐私保护 PCA（主成分分析）原型系统**。核心思想是：数据所有者（Client）将协方差矩阵加密后发送给云服务器（Server），Server 在**密文上直接执行矩阵乘法**，全程无法获知任何明文信息。最终 Client 解密后在本地完成特征提取，得到与明文 PCA 高度一致的主成分。

### 1.1 应用场景

- 医疗数据降维分析：医院不愿将患者数据明文交给第三方云平台
- 金融风控特征提取：银行对交易数据做 PCA，但计算外包到云端
- 联邦学习中的隐私特征工程：多方协同下保护原始数据隐私

### 1.2 技术栈

| 组件 | 技术选型 | 版本 |
|------|---------|------|
| 同态加密库 | Microsoft SEAL (CKKS 方案) | v4.1.2 |
| 线性代数库 | Eigen | 3.4.0 |
| 编程语言 | C++17 | — |
| 构建系统 | CMake (FetchContent 自动拉取依赖) | >= 3.16 |

---

## 2. 系统架构

### 2.1 双角色设计

系统包含两个角色，以两个 C++ 类模拟（无需网络通信）：

```
┌─────────────────────────────────┐     ┌────────────────────────────────┐
│           Client (边端)          │     │          Server (云端)          │
│                                 │     │                                │
│  持有: SecretKey (私钥)          │     │  持有: PublicKey (公钥)         │
│        PublicKey (公钥)          │     │        RelinKeys (重线性化密钥)  │
│        Encoder / Decryptor      │     │        GaloisKeys (旋转密钥)    │
│                                 │     │        Evaluator (密态计算器)   │
│  职责:                          │     │                                │
│   · 生成协方差矩阵 C             │     │  职责:                         │
│   · 加密 C 和块向量 V            │     │   · 密态矩阵乘法 enc_C * enc_V  │
│   · 解密 Server 返回的密文        │     │   · 仅接触密文, 不持有私钥      │
│   · Lanczos 校正 (明文)          │     │                                │
│   · QR 分解 / 正交化 (明文)       │     │                                │
│   · 块三对角矩阵特征分解 (明文)    │     │                                │
└─────────────────────────────────┘     └────────────────────────────────┘
```

### 2.2 信息流与隐私边界

| 传输方向 | 数据内容 | 加密状态 |
|---------|---------|---------|
| Client → Server | `enc_C`（协方差矩阵密文, d 个）, `enc_V`（块向量密文, p 个）, 公钥/计算密钥 | 密文 |
| Server → Client | `enc_W`（矩阵乘结果密文, d×p 个） | 密文 |
| Client → Server | `enc_V_{j+1}`（新基底密文, p 个）| 密文 |

**隐私保证**: Server 全程只接触密文，从不知道 C、V 或任何中间结果的真实值。

---

## 3. CKKS 同态加密参数

### 3.1 参数配置

```cpp
seal::EncryptionParameters params(seal::scheme_type::ckks);
params.set_poly_modulus_degree(8192);
params.set_coeff_modulus(CoeffModulus::Create(8192, {60, 40, 40, 60}));
double scale = pow(2.0, 40);
```

### 3.2 参数解读

| 参数 | 值 | 说明 |
|------|-----|------|
| `poly_modulus_degree` | 8192 | 多项式环维度, slot_count = 4096 >> d = 10 |
| `coeff_modulus` | {60, 40, 40, 60} | 总 200 bits < 218 bits 安全上限, 提供 2 层乘法深度 |
| `scale` | 2^40 | 浮点编码精度, 约 12 位有效数字 |
| GaloisKeys 旋转步长 | {1, 2, 4, 8} | 用于 rotate-and-sum 累加 d=10 个 slot |

### 3.3 乘法深度分析

```
密文初始层级: Level 2
  ↓  密文-密文乘法 (multiply)
  ↓  重线性化 (relinearize)
  ↓  缩放 (rescale)
密文结果层级: Level 1  ← 仍可解密
  ↓  旋转+加法 (rotate-and-sum) ← 不消耗层级
密文返回 Client: Level 1
```

每轮迭代后 Client 解密再重新加密, 深度回到 Level 2, 因此可无限迭代。

---

## 4. 数据打包策略

### 4.1 协方差矩阵 C (d×d) — 行打包

```
enc_C[0] = Encrypt([ C[0][0], C[0][1], ..., C[0][9], 0, 0, ..., 0 ])
enc_C[1] = Encrypt([ C[1][0], C[1][1], ..., C[1][9], 0, 0, ..., 0 ])
  ...                              ↑ d=10 个有效值
enc_C[9] = Encrypt([ C[9][0], C[9][1], ..., C[9][9], 0, 0, ..., 0 ])
                                              ↑ 4096 slots, 剩余填 0
```

共 d = 10 个密文, 每个密文存储 C 的一行。

### 4.2 块向量 V (d×p) — 列打包

```
enc_V[0] = Encrypt([ V[0][0], V[1][0], ..., V[9][0], 0, ..., 0 ])  ← 第 0 列
enc_V[1] = Encrypt([ V[0][1], V[1][1], ..., V[9][1], 0, ..., 0 ])  ← 第 1 列
```

共 p = 2 个密文, 每个密文存储 V 的一列。

### 4.3 设计考量

行打包 C + 列打包 V 的搭配使得 C 的第 i 行与 V 的第 j 列在 slot 层面天然对齐：

```
enc_C[i]: [ C[i][0],   C[i][1],   ..., C[i][9],   0, ... ]
enc_V[j]: [ V[0][j],   V[1][j],   ..., V[9][j],   0, ... ]
  乘积:   [ C[i][0]*V[0][j], C[i][1]*V[1][j], ..., 0, ... ]
  累加:   → slot 0 = Σ_k C[i][k]*V[k][j] = (C*V)[i][j]
```

---

## 5. 算法流程详解

### 5.1 阶段 1: 初始化 (Client)

```
1. 生成随机对称正定协方差矩阵:  C = A^T * A + I  (d×d = 10×10)
2. 行打包加密 C → enc_C (10 个密文)
3. 生成随机块向量 V1 (10×2), QR 正交归一化
4. 列打包加密 V1 → enc_V (2 个密文)
5. 创建 Server, 共享 SEALContext + PublicKey + RelinKeys + GaloisKeys
```

### 5.2 阶段 2: Block Lanczos 迭代 (3 轮 Client-Server 交互)

每一轮 j = 1, 2, 3 的流程:

```
[Server 密态计算]
  enc_W = matmul(enc_C, enc_V)
  // 对每个 (i,j) 对: enc_C[i] ⊙ enc_V[j] → relinearize → rescale → rotate-and-sum
  // 输出 d×p = 20 个标量密文
  返回 enc_W 给 Client

[Client 解密]
  W_raw = Decrypt(enc_W)  → 10×2 明文矩阵

[Client Lanczos 校正 (全部明文运算)]
  if j > 1:
      W_j = W_raw - V_{j-1} * B_{j-1}     // 减去上一步残余
  A_j = V_j^T * W_j                        // 投影系数矩阵 (2×2)
  W_new = W_j - V_j * A_j                  // 去除当前方向分量

[Client 完全正交化]
  // Double Gram-Schmidt: 对 W_new 与所有历史基 V_1,...,V_j 强制正交
  // 执行两遍以消除 HE 近似误差引起的正交性漂移
  W_hat = W_new - Σ_i V_i * (V_i^T * W_new)   (重复 2 次)

[Client QR 分解]
  V_{j+1}, B_j = QR(W_hat)
  // V_{j+1} (10×2): 归一化正交基, 作为下一轮迭代的输入
  // B_j (2×2): 上三角矩阵, Block Lanczos 的 β 参数

[Client 重新加密]
  enc_V = Encrypt(V_{j+1})   // 深度归零, 噪声清除
  发送 enc_V 给 Server, 进入下一轮
```

### 5.3 每轮重新加密的意义

| 问题 | 解决方式 |
|------|---------|
| CKKS 乘法消耗深度 | Client 解密后重加密, Level 回到最高层 |
| CKKS 近似计算引入噪声 | 重新加密 = 噪声完全清除 |
| 正交化/QR 需要除法和开方 | 在明文下用 Eigen 执行, 无同态加密限制 |

### 5.4 阶段 3: 主成分提取与验证

3 轮迭代完成后, Client 持有:
- A_1, A_2, A_3 (三个 2×2 投影矩阵)
- B_1, B_2 (两个 2×2 上三角矩阵)
- V_1, V_2, V_3 (三个 10×2 正交基)

**构建块三对角矩阵 T_m (6×6):**

```
T_m = | A_1    B_1^T        |
      | B_1    A_2    B_2^T |
      |        B_2    A_3   |
```

T_m 是原始 10×10 矩阵 C 在 Krylov 子空间上的投影。对 T_m 做特征分解:
- 提取前 K=2 个最大特征值 → 近似 C 的最大特征值
- 对应特征向量 U_K (6×2) → 通过 V_total * U_K 映射回原始 d 维空间

**验证:**
- 对 C 直接做 `Eigen::SelfAdjointEigenSolver` 得到真实特征值/向量
- 计算 Lanczos 近似结果与真实结果的**余弦相似度**

---

## 6. Server 密态矩阵乘法实现

### 6.1 内积算法 (innerProduct)

```
输入: enc_C[i] (行打包), enc_V[j] (列打包)
输出: 标量密文, slot 0 = dot(C_row_i, V_col_j)

Step 1: element-wise multiply
  product = enc_C[i] ⊙ enc_V[j]
  → slots: [C[i][0]*V[0][j], C[i][1]*V[1][j], ..., C[i][9]*V[9][j], 0, ...]

Step 2: relinearize
  将 3-元组密文缩减回 2-元组 (降低后续计算开销)

Step 3: rescale
  scale 从 2^80 降至 ~2^40, 消耗 1 层乘法深度

Step 4: rotate-and-sum (步长 1, 2, 4, 8)
  rotate by 1: [c1v1, c2v2, ..., 0, c0v0, ...]  + 原文 → 相邻对求和
  rotate by 2: 每 2 个求和
  rotate by 4: 每 4 个求和
  rotate by 8: 每 8 个求和
  → slot 0 = 全部 10 个分量之和 (slot 10~15 为 0, 不影响)
```

### 6.2 矩阵乘法 (matmul)

```
for i = 0 to d-1:        // 遍历 C 的每一行
  for j = 0 to p-1:      // 遍历 V 的每一列
    result[i*p + j] = innerProduct(enc_C[i], enc_V[j])

输出: d*p = 20 个标量密文
```

每轮迭代: 20 次密文乘法 + 80 次旋转操作。三轮共 60 次密文乘法。

---

## 7. 项目结构

```
PCA/
├── CMakeLists.txt          # 构建配置, FetchContent 自动拉取 SEAL + Eigen
├── build.sh                # 一键构建脚本
├── project.md              # 算法规格说明
├── REPORT.md               # 本文档
└── src/
    ├── main.cpp             # 三阶段主流程编排 (189 行)
    ├── client.h             # Client 类声明 (97 行)
    ├── client.cpp           # Client 类实现 (246 行)
    ├── server.h             # Server 类声明 (51 行)
    └── server.cpp           # Server 类实现 (77 行)
```

### 7.1 文件职责

| 文件 | 职责 | 关键方法 |
|------|------|---------|
| `client.h/cpp` | CKKS 上下文与密钥管理; 加密/解密; 明文侧 Lanczos 校正 | `encryptCovMatrix()`, `encryptBlockVec()`, `decryptToMatrix()`, `fullReorthogonalize()`, `qrDecompose()`, `buildBlockTridiag()` |
| `server.h/cpp` | 密态矩阵乘法 (唯一的 HE 计算) | `matmul()`, `innerProduct()` |
| `main.cpp` | 三阶段流程编排, 参数打印, 余弦相似度验证 | `main()`, `cosineSimilarity()` |

---

## 8. 构建与运行

### 8.1 前置依赖

- CMake >= 3.16
- C++17 兼容编译器 (Clang, GCC, MSVC)
- Git (用于 FetchContent 拉取依赖)

SEAL 和 Eigen 通过 CMake FetchContent **自动下载**, 无需手动安装。

### 8.2 构建

```bash
cd PCA
bash build.sh
```

首次构建约需 1-2 分钟（含依赖下载与 SEAL 编译），后续增量编译仅需数秒。

### 8.3 运行

```bash
./build/he_pca
```

### 8.4 预期输出

```
=== HE Block Lanczos PCA PoC ===
Parameters: d=10, p=2, m=3, K=2
--------------------------------------------------

[Phase 1] 初始化...
  协方差矩阵 C (10x10) 已生成
  加密 C: 10 个密文 (行打包)
  V1 (10x2) 正交归一化完成
  加密 V1: 2 个密文 (列打包)
  Server 初始化完成

[Phase 2] Block Lanczos 迭代 (3 轮)...

  --- 迭代 1/3 ---
  [Server] 计算密态矩阵乘 enc_C * enc_V ...
  [Server] 完成: 20 个点积密文
  [Client] 解密得到 W_raw (10x2)
  [Client] A_1 (2x2) 计算完成
  [Client] B_1 (2x2), V_2 (10x2) 已提取
  [Client] V_2 重新加密完成 (深度重置)

  --- 迭代 2/3 ---
  [Server] 计算密态矩阵乘 enc_C * enc_V ...
  [Server] 完成: 20 个点积密文
  [Client] 解密得到 W_raw (10x2)
  [Client] A_2 (2x2) 计算完成
  [Client] B_2 (2x2), V_3 (10x2) 已提取
  [Client] V_3 重新加密完成 (深度重置)

  --- 迭代 3/3 ---
  [Server] 计算密态矩阵乘 enc_C * enc_V ...
  [Server] 完成: 20 个点积密文
  [Client] 解密得到 W_raw (10x2)
  [Client] A_3 (2x2) 计算完成
  [Client] B_3 (2x2), V_4 (10x2) 已提取
  [Client] V_4 重新加密完成 (深度重置)

[Phase 3] 主成分提取与验证...
  块三对角矩阵 T_m (6x6) 已构建
  T_m 特征分解完成

==================================================
  结果对比 (前 2 个主成分)
==================================================
  PC1:  Lanczos 特征值 = 22.5413  |  真实特征值 = 23.5501  |  余弦相似度 = 0.9443
  PC2:  Lanczos 特征值 = 19.1706  |  真实特征值 = 19.9120  |  余弦相似度 = 0.9435

=== PoC 完成 ===
```

---

## 9. PoC 参数设定

| 参数 | 符号 | 值 | 说明 |
|------|------|-----|------|
| 数据维度 | d | 10 | 协方差矩阵大小 d×d |
| Block 大小 | p | 2 | 每次并行处理的特征数 |
| Lanczos 迭代次数 | m | 3 | Client-Server 交互轮数 |
| 提取主成分数 | K | 2 | 最终输出的 PCA 维度 |

### 9.1 计算量统计

| 阶段 | 操作 | 密文乘法次数 | 旋转次数 |
|------|------|------------|---------|
| Phase 2 每轮 | matmul(enc_C, enc_V) | d × p = 20 | 20 × 4 = 80 |
| Phase 2 三轮合计 | — | 60 | 240 |
| Phase 1 加密 | enc_C + enc_V | — | — |
| Phase 3 | 纯明文运算 | 0 | 0 |

---

## 10. 设计决策与权衡

### 10.1 "极简退化版"架构选择

本 PoC 采用了协议的简化版本:

| 完整版 (project.md 描述) | 本 PoC 实现 | 原因 |
|------------------------|------------|------|
| Server 计算 C\*V - V_prev\*B_prev | Server 只计算 C\*V, 减法在 Client 明文中完成 | 避免密文间减法的层级/scale 对齐问题 |
| Server 计算 A_j = V^T \* W (密态) | Client 明文计算 A_j | 避免额外一层密文乘法深度 |
| Server 计算 W_new = W - V\*A (密态) | Client 明文计算 | 同上 |

这种退化不影响算法的数学正确性（最终结果完全一致），只影响"Server 侧做了多少计算"的分工。对于 PoC 验证算法可行性来说是最优选择。

### 10.2 为什么不用 SIMD 对角线打包？

对于大规模矩阵（d=1000+），通常使用"对角线打包 + 旋转"的方法把整个矩阵编码到 1-2 个密文中，用 O(d) 次旋转完成矩阵-向量乘法。但对于 d=10 的 PoC:

- 对角线方法的代码复杂度远高于朴素方法
- d=10 时朴素方法仅需 20 次密文乘法, 完全可接受
- 优先保证代码可读性和正确性

---

## 11. 可扩展方向

1. **增大维度**: 将 d 提升至 100+, 测试 CKKS 在更大规模下的精度和性能
2. **SIMD 优化**: 使用对角线打包法将矩阵乘法从 O(d²) 次密文乘法降至 O(d) 次
3. **Server 侧完整计算**: 将 V_prev\*B_prev 减法和 A_j 计算也移至 Server 密态完成
4. **Bootstrapping**: 引入 CKKS bootstrapping 消除乘法深度限制, 支持任意多轮迭代
5. **网络通信**: 用 gRPC / ZeroMQ 替代类间调用, 实现真正的分布式部署
6. **基准测试**: 对比不同 poly_modulus_degree (4096/8192/16384) 下的精度-性能权衡
