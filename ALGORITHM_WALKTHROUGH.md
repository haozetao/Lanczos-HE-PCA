# HE-PCA 算法走读：从零开始用一个 d=4 的例子贯穿整条流水线

> 这份文档是给**第一次接触同态加密 + PCA**的读者准备的。每一步都有：
> 1. **数学公式**——告诉你这一步在算什么
> 2. **代码片段**——指向项目里的真实实现
> 3. **手算数值**——用一个 4×4 的小矩阵让你能用纸笔跟下来
>
> 阅读路径：第一、二部分是必备背景（如果你已经熟悉 PCA 和 CKKS，可以跳）。第三部分讲整个协议怎么拼起来。**第四部分是核心**——把同一个数据从客户端原始矩阵一直算到客户端拿到主成分，每一步都给数。第五部分把这个例子推广到真实大维度时会遇到的所有问题。第六、七部分是代码地图和 FAQ。

---

## 目录

1. [PCA 在做什么](#1-pca-在做什么)
2. [CKKS 同态加密极简介](#2-ckks-同态加密极简介)
3. [协议骨架：谁做什么、通信几轮](#3-协议骨架谁做什么通信几轮)
4. [完整走例（d=4 全程手算）](#4-完整走例d4-全程手算)
5. [放大到真实维度后会出现什么问题](#5-放大到真实维度后会出现什么问题)
6. [代码导航地图](#6-代码导航地图)
7. [FAQ 常见疑问](#7-faq-常见疑问)

---

## 1. PCA 在做什么

### 1.1 一句话

PCA = "在高维数据云里找出**最分散的几条主轴**"。

### 1.2 几何直觉

想象你有一堆二维点散布如下（每个点是一个样本）：

```
y                  o
↑               o   o  o
|             o    .  o   o
|         o   .       .  o
|       o  .    .  .       o
|     o   .  .   .       o
|   o   .   .  .     o
|       .  .
+----------------------→ x
```

这堆点显然有一条"主方向"（左下到右上），数据沿这个方向最分散；它的垂直方向数据几乎没什么变化。PCA 的任务就是**自动找出这条主方向**——不需要你人为去看图。

把这个想法推广到高维：N 个样本、每个 d 维，PCA 找出排序的 K 条正交主轴 (v₁, v₂, ..., v_K)，让数据沿 v₁ 最分散、v₂ 次之...，每条主轴的"分散程度"由对应的特征值 λᵢ 度量（数学上就是方差）。

### 1.3 数学定义

给定数据矩阵 X ∈ ℝ^{N×d}（N 行样本，d 列特征）：

**Step 1**：中心化（每列减去均值）
\[
X_c = X - \mathbf{1}\bar{x}^\top
\]

**Step 2**：协方差矩阵
\[
C = \frac{1}{N-1} X_c^\top X_c \in \mathbb{R}^{d \times d}
\]
- C 一定是**对称半正定**矩阵（C = Cᵀ, 所有特征值 ≥ 0）
- 这是后续所有 PCA 计算的核心对象

**Step 3**：求 C 的特征分解
\[
C v_i = \lambda_i v_i, \quad \lambda_1 \ge \lambda_2 \ge \dots \ge \lambda_d \ge 0
\]
- vᵢ 是单位向量（‖vᵢ‖=1），互相正交（vᵢ · vⱼ = 0, i ≠ j）
- λᵢ 是数据沿 vᵢ 方向的方差（即"分散程度"）
- 通常我们只要前 K 个：V_K = [v₁, ..., v_K] ∈ ℝ^{d×K}

### 1.4 PCA 的输出有什么用

| 应用 | 公式 | 说明 |
|---|---|---|
| **降维** | Z = X_c · V_K | 每个样本从 d 维压成 K 维 |
| **重建（lossy）** | X̂ = Z · V_Kᵀ | 用 K 个主成分近似还原原数据 |
| **可视化** | K=2 或 3 时画 Z | 看高维数据的内在结构 |
| **特征工程** | 把 Z 喂给下游 ML 模型 | 比原始 d 维特征更紧凑、更鲁棒 |

衡量 PCA 好坏的核心指标 **R²(X)**：
\[
R^2(X) = 1 - \frac{\|X_c - Z V_K^\top\|^2}{\|X_c\|^2}
\]
表示"前 K 个主成分能解释多少信息量"。R²(X) → 1 表示 K 个主成分几乎没丢信息。

### 1.5 一个最小例子（4×4 协方差矩阵）

我们后面会全程用这个例子。设：
\[
C = \begin{pmatrix} 4 & 1 & 0 & 0 \\ 1 & 4 & 0 & 0 \\ 0 & 0 & 2 & 0 \\ 0 & 0 & 0 & 1 \end{pmatrix}
\]

C 是块对角矩阵，可以**手算特征值/特征向量**：

- 上 2×2 块 [[4,1],[1,4]]：特征值 5、3
  - λ=5 对应 (1,1)/√2（解 (4-5)x₁+x₂=0 → x₁=x₂）
  - λ=3 对应 (1,-1)/√2
- 下两个对角元独立：λ=2 对应 (0,0,1,0)；λ=1 对应 (0,0,0,1)

所以 C 的完整谱：

| i | λᵢ | vᵢ |
|---|---|---|
| 1 | **5** | **(1, 1, 0, 0) / √2** |
| 2 | **3** | **(1, −1, 0, 0) / √2** |
| 3 | 2 | (0, 0, 1, 0) |
| 4 | 1 | (0, 0, 0, 1) |

这是真值（"明文里直接调 SelfAdjointEigenSolver"会得到的答案），后面我们会用密态 Lanczos 流程算到几乎一样的结果。

---

## 2. CKKS 同态加密极简介

### 2.1 为什么需要

PCA 的输入 C（或更原始的 X）通常是隐私数据（医疗记录、用户行为...）。我们希望 Server **看不到 C 的明文**也能算出 (λ, v)。"看不到明文还能算"这个能力就是**同态加密 (HE)**。

CKKS 是 HE 的一种方案，专门为**实数和复数**设计（其它方案如 BGV/BFV 只能算整数）。

### 2.2 CKKS 的核心抽象

| 角色 | 数学对象 | 在我们项目里 |
|---|---|---|
| **明文 (plaintext)** | 一组实数 (a₀, a₁, ..., a_{n−1})，n=numSlots | 一个 d 维向量塞进前 d 个 slot，剩下补 0 |
| **密文 (ciphertext)** | 一对多项式环上的元素 (b, c) ∈ R_q² | 加密后的不可读对象，C++ 类型 `Ciphertext<DCRTPoly>` |
| **加密** | Enc(pt, pk) → ct | `cc->Encrypt(pk, pt)` |
| **解密** | Dec(ct, sk) → pt' ≈ pt | `cc->Decrypt(sk, ct, &pt)` |

**关键：CKKS 是"近似"加密**——解密后得到的不是 pt 而是 pt' = pt + 微小噪声 ε。对实数运算这很好（误差通常 < 10⁻⁶），但对整数应用（比如查表）可能出问题。

### 2.3 三个基本同态原语

所有 CKKS 上的运算都是这三个原语的组合：

| 原语 | 数学含义 | OpenFHE API |
|---|---|---|
| **EvalAdd(ct₁, ct₂)** | slot-wise 加 | 廉价，几乎不消耗"层数" |
| **EvalMult(ct₁, ct₂)** | slot-wise 乘 | **每次消耗 1 层"乘法深度"** |
| **EvalRotate(ct, k)** | slot 数组循环左移 k 位 | 不消耗深度，但需要"旋转密钥" |

**例子：** 一个 4-slot CKKS，明文 (3, 5, 7, 11)，加密后做 `EvalRotate(ct, 1)`，解密回来是 (5, 7, 11, 3)。

### 2.4 乘法深度 = 一个最大可消耗的"预算"

每次 `EvalMult` 会让密文"消耗 1 层"。系统在生成密钥时设定一个 **multiplicativeDepth**（比如 48），密文最多累计做 48 次乘法。**到了上限再乘就会丢失精度，最终解密失败**。

为什么？CKKS 内部用一条"模数链" q_L > q_{L-1} > ... > q_0，每次乘法把级别从 L 降到 L−1（rescale）。链长完了就没法继续。

### 2.5 救命操作：Bootstrap

**Bootstrap = "把已经消耗了很多层的密文刷新回最高层"**。这是 HE 真正能算"任意深的电路"的核心 trick。

- **优点**：让一个深电路（远超 multiplicativeDepth）变得可行
- **代价**：单次 BS 在 RingDim=32768 + multiplicativeDepth=48 下要 ~3.5–4 秒，是单次普通乘法的 ~30 倍

我们在 Lanczos 主循环里**每步进入时检查剩余深度**，不够就 BS V/V_prev/β_prev 一次，让下一步的整条电路有足够"层数预算"。

### 2.6 数据打包 (Packing)

一个 RingDim=32768 的 CKKS 密文同时承载 16384 个 slot 的实数——你可以把整个 d=784 的向量塞进一个密文，不必加 784 个标量密文。`EvalMult` 是 slot-wise 的，所以 (a₀,a₁,...) ⊙ (b₀,b₁,...) 一次就并行算完所有元素积。

我们项目里：
- 一个 d 维向量 V → 1 个密文（前 d 个 slot 装数据，剩下补 0）
- 一个 d×d 矩阵 C → **d 个密文**（每行装一个密文）

---

## 3. 协议骨架：谁做什么、通信几轮

### 3.1 角色与信任模型

| 角色 | 拥有 | 不能看到 |
|---|---|---|
| **Client（数据持有方）** | 私钥 sk、X、C（明文）| 无（自己的数据） |
| **Server（计算方）** | 公钥 pk、enc_C、enc_v₀（密文）| C 的明文，X 的明文 |

Server **半诚实**：会按协议执行，但会试图从看到的密文里推出明文（CKKS 在标准安全级 HEStd_128_classic 下，被严格证明无法做到这一点）。

### 3.2 通信轮数：仅 2 轮

```
Client                                  Server
  │
  │ ── enc_C, enc_v₀ ──────────────────►│
  │                                     │ Lanczos m 步（密态）
  │                                     │   - matvec
  │                                     │   - inner product
  │                                     │   - Newton 1/√x
  │                                     │   - FRO 重正交
  │                                     │   - 按需 Bootstrap
  │ ◄─────────── α[m], β[m-1], V_all[m] │
  │
  │ 解密 + CW 滤波 + 特征向量重构
  │
  └─→ 输出 (λ₁..λ_K, v₁..v_K)
```

**为什么只 2 轮**：传统密态 PCA 用 power iteration，每次归一化都要把 ‖x‖ 解密回 Client（K × T 轮，T 是收敛步数，可能数十轮）。我们把 Newton 1/√x 直接做在密文上，省掉所有迭代里的 round trip。

### 3.3 为什么用 Lanczos 而不是 Power Iteration

| 维度 | Power Iteration | **Lanczos** |
|---|---|---|
| 每次拿到几个特征对 | 1 | **K（同时）** |
| 拿 K 个需要的迭代轮 | K × T（T~50） | **m ≥ K（一次）** |
| 通信轮数 | O(K·T) | **O(1)** |
| 单步乘法深度 | 4–8 层 | 12–26 层（含 FRO） |
| 数值稳定性 | 强（自带归一化）| 弱（需要重正交）|

Lanczos 的最大优势是**单轮通信拿全部 K**——这是把通信从 O(K·T) 降到 O(1) 的关键。代价是必须解决正交性丢失，这是后面 FRO 章节的主题。

---

## 4. 完整走例（d=4 全程手算）

### 4.1 设定

- 协方差矩阵 C 用 [§1.5](#15-一个最小例子4x4-协方差矩阵) 那个 4×4 矩阵
- m_iter = 2（Lanczos 跑 2 步）
- K = 2（提取前 2 个主成分）
- 启动向量 v₀ = (1, 0, 0, 0)
- 暂不考虑 CKKS 噪声（就当加密/解密是无损的；这样手算结果干净）
- 数据流照着 `src/main.cpp::runOnce` 走

我们要验证：**密态 Lanczos 算出 λ_HE × trace_C ≈ 真值 5 和 3**。

### 4.2 Client 第一步：构造 C 的明文

代码：

```119:120:src/main.cpp
        std::cout << "  数据: 随机协方差 A^T·A + I" << std::endl;
        client.generateCovarianceMatrix();
```

实际项目里通常用 `generateLowRankDataset` 从样本数据 X 里算 C，但走例里我们直接给 C：

```python
C = [[4, 1, 0, 0],
     [1, 4, 0, 0],
     [0, 0, 2, 0],
     [0, 0, 0, 1]]
```

### 4.3 归一化：C ← C / trace(C)

为什么：CKKS 在数值范围 |x| < 1 时精度最高。归一化让 C 的特征值落进 [0, 1]。

代码：

```602:610:src/client.cpp
double Client::normalizeCovariance()
{
    trace_C_ = C_.trace();
    if (trace_C_ < 1e-15) {
        throw std::runtime_error("normalizeCovariance: trace(C) is near zero");
    }
    C_ /= trace_C_;
    return trace_C_;
}
```

手算：

- trace(C) = 4 + 4 + 2 + 1 = **11**
- Ĉ = C / 11

\[
\hat{C} = \tfrac{1}{11}\begin{pmatrix} 4 & 1 & 0 & 0 \\ 1 & 4 & 0 & 0 \\ 0 & 0 & 2 & 0 \\ 0 & 0 & 0 & 1 \end{pmatrix}
\]

Ĉ 的特征值变成 5/11, 3/11, 2/11, 1/11（精确除 11），特征向量不变。

### 4.4 加密

代码：

```151:164:src/client.cpp
std::vector<Ciphertext<DCRTPoly>> Client::encryptCovMatrix() const
{
    std::vector<Ciphertext<DCRTPoly>> enc_C(d_);

    for (int i = 0; i < d_; ++i) {
        std::vector<double> row(num_slots_, 0.0);
        for (int k = 0; k < d_; ++k) {
            row[k] = C_(i, k);
        }
        Plaintext pt = cc_->MakeCKKSPackedPlaintext(row);
        enc_C[i] = cc_->Encrypt(keys_.publicKey, pt);
    }
    return enc_C;
}
```

我们得到 `enc_C[0..3]` 共 4 个密文：

```
enc_C[0] ← 加密([4/11, 1/11, 0, 0, 0, ..., 0])    ← C 第 0 行
enc_C[1] ← 加密([1/11, 4/11, 0, 0, 0, ..., 0])
enc_C[2] ← 加密([0, 0, 2/11, 0, 0, ..., 0])
enc_C[3] ← 加密([0, 0, 0, 1/11, 0, ..., 0])
```

每个密文有 numSlots=16384 个 slot；前 4 个装 C 的对应行，剩 16380 个 slot 全是 0。

同样把 v₀ 加密成 1 个密文：
```
enc_v ← 加密([1, 0, 0, 0, 0, ..., 0])
```

发到 Server。

### 4.5 Server 端 Lanczos 第 0 步 (j=0)

代码主循环：

```172:227:src/server.cpp
    for (int iter = 0; iter < m_iter; ++iter) {
        if (bootstrap_enabled_ && iter > 0) {
            bootstrapIfNeeded(V, bootstrap_threshold, stats_out);
            ...
        }

        out.V_all.push_back(V);

        // Step 1: W = C · V  （逐行 innerProduct 后 pack）
        std::vector<Ciphertext<DCRTPoly>> scalars(d);
        for (int i = 0; i < d; ++i) {
            scalars[i] = innerProduct(enc_C[i], V, d);
        }
        Ciphertext<DCRTPoly> Wvec = packScalarsToVector(scalars, d);

        // Step 1b: W = W − β_{j-1} · v_{j-1}
        if (iter > 0) {
            auto bv = scalarVecMultiply(beta_prev_ct, V_prev);
            Wvec = cc_->EvalSub(Wvec, bv);
        }

        // Step 2: α = V^T · W
        auto alpha = innerProduct(V, Wvec, d);
        ...
    }
```

#### 4.5.1 matvec：W = Ĉ · V

`V = enc_v0 = [1, 0, 0, 0, ...]`

逐行做 d 次 inner product：

| i | enc_C[i] 装的内容 | innerProduct(enc_C[i], V) 结果（slot 0..3）|
|---|---|---|
| 0 | [4/11, 1/11, 0, 0] | (4/11)·1 + 0 + 0 + 0 = **4/11** |
| 1 | [1/11, 4/11, 0, 0] | (1/11)·1 + 0 + 0 + 0 = **1/11** |
| 2 | [0, 0, 2/11, 0] | 0·1 + 0 + 0 + 0 = **0** |
| 3 | [0, 0, 0, 1/11] | 0·1 + 0 + 0 + 0 = **0** |

**注意**：innerProduct 返回的密文里**所有 slot 都装着同一个标量值**——这是 rotate-and-sum 的结果（[§4.5.2](#452-innerproduct-内部到底干了什么)）。

接着 `packScalarsToVector` 把 4 个标量装回到一个向量密文里：

```
W = pack([4/11, 1/11, 0, 0]) ← 加密
```

具体怎么 pack：用 mask `[1,0,0,0]` 把 scalars[0] 提取到 slot 0，用 mask `[0,1,0,0]` 把 scalars[1] 提取到 slot 1，依此类推，再全部加起来。

#### 4.5.2 innerProduct 内部到底干了什么

```48:64:src/server.cpp
Ciphertext<DCRTPoly> Server::innerProduct(
    const Ciphertext<DCRTPoly>& a,
    const Ciphertext<DCRTPoly>& b,
    int /*dim*/) const
{
    // 逐元素乘积
    auto product = cc_->EvalMult(a, b);

    // 全 slot rotate-and-sum：旋转步长覆盖到 num_slots_/2
    for (uint32_t step = 1; step < num_slots_; step *= 2) {
        auto rotated = cc_->EvalRotate(product, static_cast<int32_t>(step));
        product = cc_->EvalAdd(product, rotated);
    }
    return product;
}
```

举一个简化版：假设 numSlots=4，a=(a₀,a₁,a₂,a₃)，b=(b₀,b₁,b₂,b₃)。

```
product = a ⊙ b = (a₀b₀, a₁b₁, a₂b₂, a₃b₃)

step=1：rotated = (a₁b₁, a₂b₂, a₃b₃, a₀b₀)
        product = (a₀b₀+a₁b₁, a₁b₁+a₂b₂, a₂b₂+a₃b₃, a₃b₃+a₀b₀)

step=2：rotated = (a₂b₂+a₃b₃, a₃b₃+a₀b₀, a₀b₀+a₁b₁, a₁b₁+a₂b₂)
        product 每个 slot = a₀b₀+a₁b₁+a₂b₂+a₃b₃ ← 所有 slot 都是同一个标量
```

需要 log₂(numSlots) 次旋转。我们用 numSlots=16384 → **每次 inner product 需要 14 次 EvalRotate + 1 次 EvalMult**。

#### 4.5.3 计算 α₀ = V·W

```
W = [4/11, 1/11, 0, 0]
V = [1,    0,    0, 0]
α₀ = V·W = 1·(4/11) + 0·(1/11) + 0 + 0 = 4/11
```

代码：

```198:200:src/server.cpp
        // Step 2: α = V^T · W
        auto alpha = innerProduct(V, Wvec, d);
        out.alphas.push_back(alpha);
```

#### 4.5.4 W_new = W − α₀·V

代码：

```203:206:src/server.cpp
        if (!is_last) {
            // Step 3: W_new = W − α · V
            auto alphaV = scalarVecMultiply(alpha, V);
            auto Wnew = cc_->EvalSub(Wvec, alphaV);
```

`scalarVecMultiply(alpha, V)` 是 slot-wise 乘：每个 slot 装着 α₀ 的那个密文，逐 slot 乘 V，得到 α₀·V。

```
α₀·V = (4/11)·[1, 0, 0, 0] = [4/11, 0, 0, 0]
W_new = W − α₀·V = [4/11 − 4/11, 1/11, 0, 0] = [0, 1/11, 0, 0]
```

#### 4.5.5 是否做 FRO

代码：

```208:218:src/server.cpp
            // Step 3b (Optional): Full Reorthogonalization (HE-FRO)
            if (enable_fro && iter >= fro_skip) {
                for (int k = 0; k < iter; ++k) {
                    const auto& Vk = out.V_all[k];
                    auto proj_k = innerProduct(Vk, Wnew, d);
                    auto sub_k = scalarVecMultiply(proj_k, Vk);
                    Wnew = cc_->EvalSub(Wnew, sub_k);
                }
            }
```

j=0 时：`iter=0 < fro_skip=2`，跳过 FRO（这一步不可能有正交性丢失，因为只有 V₀ 一个向量）。

#### 4.5.6 Newton 1/√x 算 β₀

需要算 β₀ = ‖W_new‖ = √(W_new · W_new)。直接开方在 CKKS 里很贵，所以拆成：先算 ‖W_new‖²，再调 Newton 迭代算 1/√(·)。

```
‖W_new‖² = 0² + (1/11)² + 0 + 0 = 1/121 ≈ 0.00826
β₀ = √(1/121) = 1/11 ≈ 0.0909
1/β₀ = 11
```

代码：

```209:224:src/server.cpp
            // Step 4: β = ||W_new|| via Newton 1/√x
            auto norm_sq = innerProduct(Wnew, Wnew, d);
            ...
            InvSqrtResult inv = use_asor
                ? newton_->computeWithASOR(norm_sq, guess_inv_sqrt, asor_k_factors)
                : newton_->compute(norm_sq, guess_inv_sqrt, newton_iters);
            out.betas.push_back(inv.sqrt_val);
```

Newton 1/√x 的迭代公式（标准 Newton-Raphson 求 f(y) = 1/y² − x = 0）：
\[
y_{k+1} = \frac{y_k(3 - x \cdot y_k^2)}{2}
\]

它收敛到 1/√x，然后 √x = x · (1/√x)。

具体怎么选初值见 [§5.3](#53-newton-1√x-怎么在密文里做)。这一步走完得到密文形式的 β₀ = 1/11 和 1/β₀ = 11。

#### 4.5.7 V₁ = W_new × (1/β₀)

```
V₁ = [0, 1/11, 0, 0] × 11 = [0, 1, 0, 0]
```

代码：

```222:225:src/server.cpp
            // Step 5: v_{j+1} = W_new * (1/β)
            V_prev = V;
            beta_prev_ct = inv.sqrt_val;
            auto Vnext = scalarVecMultiply(inv.inv_sqrt, Wnew);
            V = std::move(Vnext);
```

**第 0 步走完，状态：**
- α₀ = 4/11
- β₀ = 1/11
- V₀ = [1, 0, 0, 0]（已存进 V_all[0]）
- V_prev = V₀
- V = V₁ = [0, 1, 0, 0]

### 4.6 Server 端 Lanczos 第 1 步 (j=1)

#### 4.6.1 进入循环时按需 Bootstrap

```173:181:src/server.cpp
        if (bootstrap_enabled_ && iter > 0) {
            bootstrapIfNeeded(V, bootstrap_threshold, stats_out);
            if (iter > 0 && V_prev) {
                bootstrapIfNeeded(V_prev, bootstrap_threshold, stats_out);
            }
            if (beta_prev_ct) {
                bootstrapIfNeeded(beta_prev_ct, bootstrap_threshold, stats_out);
            }
        }
```

j=1 时，V/V_prev/β_prev 都已经消耗了若干层；如果剩余深度 < threshold 就 Bootstrap。

V_all[1] 在 BS 之后才 push（保证存进去的是"刷新过的、剩余深度高"的密文，方便后续 reorth 复用）。

#### 4.6.2 matvec: W = Ĉ · V₁

V₁ = [0, 1, 0, 0]，逐行 inner product：

| i | enc_C[i] | scalars[i] |
|---|---|---|
| 0 | [4/11, 1/11, 0, 0] | (4/11)·0 + (1/11)·1 + 0 + 0 = **1/11** |
| 1 | [1/11, 4/11, 0, 0] | (1/11)·0 + (4/11)·1 + 0 + 0 = **4/11** |
| 2 | [0, 0, 2/11, 0] | 0 + 0 + 0 + 0 = **0** |
| 3 | [0, 0, 0, 1/11] | 0 + 0 + 0 + 0 = **0** |

```
Ĉ·V₁ = [1/11, 4/11, 0, 0]
```

#### 4.6.3 W -= β₀·V₀

```
β₀·V₀ = (1/11)·[1, 0, 0, 0] = [1/11, 0, 0, 0]
W = [1/11 − 1/11, 4/11, 0, 0] = [0, 4/11, 0, 0]
```

注意：这一步对应数学公式 \( W_j = \hat{C} V_j - \beta_{j-1} V_{j-1} \)。Lanczos 的"三项递推"就是这个减法。

#### 4.6.4 α₁ = V₁ · W

```
α₁ = [0, 1, 0, 0] · [0, 4/11, 0, 0] = 0 + 1·(4/11) + 0 + 0 = 4/11
```

#### 4.6.5 j=1 是最后一步，停

```202:226:src/server.cpp
        const bool is_last = (iter == m_iter - 1);
        if (!is_last) {
            ...
        }
```

is_last=true，跳过 W_new、β、V_next 的所有计算。最后一步只算 α，因为 β 和 V 之后用不上。

**第 1 步走完，状态：**
- α₀ = 4/11，α₁ = 4/11
- β₀ = 1/11
- V_all = [V₀, V₁] = ([1,0,0,0], [0,1,0,0])

Server 把这堆密文打包发回 Client。

### 4.7 Client 解密 α/β

代码：

```213:259:src/client.cpp
Eigen::MatrixXd Client::buildTridiagonalFromEncrypted(
    const std::vector<Ciphertext<DCRTPoly>>& enc_alphas,
    const std::vector<Ciphertext<DCRTPoly>>& enc_betas,
    int m) const
{
    ...
    auto safeDecryptScalar = [this](
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct) -> double {
        try {
            Plaintext pt;
            cc_->Decrypt(keys_.secretKey, ct, &pt);
            const std::vector<double>& vals = pt->GetRealPackedValue();
            return vals.empty() ? 0.0 : vals[0];
        } catch (const lbcrypto::OpenFHEException&) {
            return 0.0;
        }
    };
    ...
```

每个 enc_α/enc_β 解密后是一个 numSlots 维的向量，前几个 slot 都装着同一个标量（rotate-and-sum 让所有 slot 一样），我们取 slot[0]。

得到：
- α[0] = 4/11 ≈ 0.3636
- α[1] = 4/11 ≈ 0.3636
- β[0] = 1/11 ≈ 0.0909

### 4.8 三对角矩阵 + 特征分解

代码：

```250:258:src/client.cpp
    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(m, m);
    for (int i = 0; i < m; ++i) {
        T(i, i) = alphas[i];
        if (i < m - 1) {
            T(i, i + 1) = betas[i];
            T(i + 1, i) = betas[i];
        }
    }
    return T;
}
```

构建：
\[
T_2 = \begin{pmatrix} \alpha_0 & \beta_0 \\ \beta_0 & \alpha_1 \end{pmatrix} = \begin{pmatrix} 4/11 & 1/11 \\ 1/11 & 4/11 \end{pmatrix}
\]

求 T_2 的特征值（明文里调 SelfAdjointEigenSolver）：

```
trace(T_2) = 8/11
det(T_2) = (4/11)·(4/11) − (1/11)·(1/11) = 16/121 − 1/121 = 15/121
```

特征值：
\[
\lambda = \frac{\text{trace} \pm \sqrt{\text{trace}^2 - 4\,\text{det}}}{2} = \frac{8/11 \pm \sqrt{64/121 - 60/121}}{2} = \frac{8/11 \pm 2/11}{2} = \tfrac{5}{11}, \tfrac{3}{11}
\]

特征向量：

| λ_T | (T_2 − λI)x = 0 → 解 | 单位向量 |
|---|---|---|
| 5/11 | (-1/11)x₁ + (1/11)x₂ = 0 → x₁ = x₂ | u₁ = (1, 1)/√2 |
| 3/11 | (1/11)x₁ + (1/11)x₂ = 0 → x₁ = −x₂ | u₂ = (1, −1)/√2 |

### 4.9 CW 滤波

代码：

```521:580:src/client.cpp
CWFilterResult Client::cullumWilloughbyFilter(const Eigen::MatrixXd& T_m, int K) const
{
    int m = static_cast<int>(T_m.rows());
    ...
    Eigen::MatrixXd T_s = T_m.bottomRightCorner(m - 1, m - 1);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_sub(T_s);
    Eigen::VectorXd evals_sub = solver_sub.eigenvalues();

    double tolerance = 1e-6 * std::max(1.0, T_m.norm());
    std::vector<int> good_indices;

    for (int i = 0; i < m; ++i) {
        double lambda = evals_full(i);
        bool is_ghost = false;
        for (int j = 0; j < m - 1; ++j) {
            if (std::abs(lambda - evals_sub(j)) < tolerance) {
                is_ghost = true;
                break;
            }
        }
        if (!is_ghost) {
            good_indices.push_back(i);
        }
    }
    ...
```

CW 算法：
1. 算 T_2 全谱 evals_full = [3/11, 5/11]
2. 算 T_2 的"去首行首列"子矩阵 T_s = (4/11)（1×1）的谱 evals_sub = [4/11]
3. 对每个 evals_full[i]，看是否"与 evals_sub 中某个值几乎相等" → 是则判为"鬼影"丢弃

我们的 5/11 和 3/11 都 ≠ 4/11，**没有鬼影**，全部保留。

CW 在 m 较小、维度低的情况下基本是 no-op，但在 m=8 高维场景里能救命（[§5.2](#52-当-m4lanczos-失去正交性fro-介入)）。

### 4.10 解密 V_all + QR 重正交

代码：

```612:631:src/client.cpp
Eigen::MatrixXd Client::decryptLanczosVectors(
    const std::vector<Ciphertext<DCRTPoly>>& V_all) const
{
    const int m = static_cast<int>(V_all.size());
    Eigen::MatrixXd V_total(d_, m);

    for (int j = 0; j < m; ++j) {
        try {
            Plaintext pt;
            cc_->Decrypt(keys_.secretKey, V_all[j], &pt);
            const std::vector<double>& vals = pt->GetRealPackedValue();
            for (int i = 0; i < d_; ++i) {
                V_total(i, j) = (i < static_cast<int>(vals.size())) ? vals[i] : 0.0;
            }
        } catch (const lbcrypto::OpenFHEException&) {
            for (int i = 0; i < d_; ++i) V_total(i, j) = 0.0;
        }
    }
    return V_total;
}
```

解密 V_all = [V₀, V₁]，得到 4×2 明文矩阵：

\[
V_{\text{total}} = \begin{pmatrix} 1 & 0 \\ 0 & 1 \\ 0 & 0 \\ 0 & 0 \end{pmatrix}
\]

QR 重正交（修复 CKKS 引入的微小数值漂移）：

```646:658:src/client.cpp
    Eigen::MatrixXd Q(d_, m);
    for (int j = 0; j < m; ++j) {
        Eigen::VectorXd v = V_total_raw.col(j);
        for (int k = 0; k < j; ++k) {
            v -= Q.col(k) * (Q.col(k).dot(v));
        }
        double nrm = v.norm();
        if (nrm > 1e-14) {
            Q.col(j) = v / nrm;
        } else {
            Q.col(j).setZero();
        }
    }
```

我们的例子里 V₀ ⊥ V₁ 已经成立、模长都是 1，QR 得到 Q = V_total。

### 4.11 主成分重构：U_K = Q · U_T

```660:675:src/client.cpp
    const Eigen::MatrixXd& S = cw.good_eigenvectors;
    Eigen::MatrixXd U(d_, take);
    for (int i = 0; i < take; ++i) {
        Eigen::VectorXd s_i = S.col(i);
        if (s_i.size() > m) {
            s_i = s_i.head(m);
        }
        Eigen::VectorXd u = Q.leftCols(s_i.size()) * s_i;
        double nrm = u.norm();
        if (nrm > 1e-14) {
            U.col(i) = u / nrm;
        } else {
            U.col(i).setZero();
        }
    }
```

U_T = [u₁, u₂] = [(1,1)/√2, (1,−1)/√2]（CW 输出的 good_eigenvectors，按降序排）

\[
\text{主成分 } v_1^{HE} = Q \cdot u_1 = \begin{pmatrix} 1 & 0 \\ 0 & 1 \\ 0 & 0 \\ 0 & 0 \end{pmatrix} \cdot \frac{1}{\sqrt{2}}\begin{pmatrix} 1 \\ 1 \end{pmatrix} = \frac{1}{\sqrt{2}}\begin{pmatrix} 1 \\ 1 \\ 0 \\ 0 \end{pmatrix}
\]

\[
v_2^{HE} = Q \cdot u_2 = \frac{1}{\sqrt{2}}\begin{pmatrix} 1 \\ −1 \\ 0 \\ 0 \end{pmatrix}
\]

### 4.12 反归一化与最终输出

```272:273:src/main.cpp
    // 反归一化后的 HE 特征值
    Eigen::VectorXd he_evals = cw.good_eigenvalues * trace_C;
```

\[
\lambda_1^{HE} = \tfrac{5}{11} \times 11 = \mathbf{5}
\]
\[
\lambda_2^{HE} = \tfrac{3}{11} \times 11 = \mathbf{3}
\]

### 4.13 验证：与真值对比

| i | 真值 λᵢ | HE 算出的 λᵢ | 真值 vᵢ | HE 算出的 vᵢ | cos_sim |
|---|---|---|---|---|---|
| 1 | 5 | 5 | (1,1,0,0)/√2 | (1,1,0,0)/√2 | **1.000** |
| 2 | 3 | 3 | (1,−1,0,0)/√2 | (1,−1,0,0)/√2 | **1.000** |

完美。这就是整条流水线在没有 CKKS 噪声、初始向量"幸运"（正好覆盖目标子空间）时的理想结果。

### 4.14 一个有意思的现象：v₃, v₄ 没拿到

注意我们的 v₀ = (1,0,0,0) 完全位于 span{v₁, v₂} 里（因为 v₁=(1,1,0,0)/√2 和 v₂=(1,-1,0,0)/√2 的和是 (1,0,0,0)·√2，所以 v₀ = (v₁+v₂)/√2）。

Lanczos 是 **Krylov subspace method**：m 步只能在 span{v₀, Ĉv₀, Ĉ²v₀, ..., Ĉ^{m-1}v₀} 里搜索。如果 v₀ 在某个特征向量方向上的投影是 0，Lanczos 永远拿不到那个特征向量。

这就是为什么真实场景里我们用**随机初始向量**——它在所有特征向量方向上几乎肯定都有非零投影。

---

## 5. 放大到真实维度后会出现什么问题

### 5.1 K 与 m 的关系

| 配置 | 数值精度上能拿出的 K | 能不能拿出 K 个**互相正交**的好特征对 |
|---|---|---|
| m = K | K（恰好够） | 数值上**不能**——没有任何冗余步 |
| m = K + 2 | K | 弱冗余，对纯净数据可行；噪声/CKKS 一上来就崩 |
| **m = 2K**（推荐） | K | **够用**，每个特征对至少有 1-2 步冗余 |

实测我们项目里：
- d=4 例子：K=2 + m=2，因为 v₀ 完美匹配子空间，刚好能跑（[§4.14](#414-一个有意思的现象v_3-v_4-没拿到)）
- d=50, K=4：m=8 推荐；m=5 时 cos₃/cos₄ 严重退化

### 5.2 当 m≥4：Lanczos 失去正交性，FRO 介入

#### 5.2.1 问题：Lanczos 的浮点 / CKKS 噪声会让 V_j 们慢慢"对齐"

理论上 Lanczos 三项递推（W = ĈV − β·V_prev、α = V·W、Wnew = W − αV）**精确算时** V_0..V_{m-1} 互相正交。但有限精度下：

- 浮点 IEEE-754 每步引入 O(2⁻⁵²) 相对误差，乘 d 维内积放大 √d 倍
- CKKS 每步引入 O(2⁻²⁰) 相对误差（比浮点差 32 个二进制位）
- 这些误差让 Wnew 在 V_0..V_{j-1} 上的"不完美正交分量"被一步步保留并放大

到 j=4-5 步时：V_4 不再 ⊥ V_0，T 的特征向量 u 在不正交基底下展开后**完全错位**——cos_sim 从 ~0.99 跌到 0.4。

#### 5.2.2 实测数据（我们项目里）

d=50, m=8, K=4, FRO=OFF：

| i | λᵢ 误差 | cos_simᵢ |
|---|---|---|
| 1 | 2.79% | 0.999 |
| 2 | 3.81% | 0.977 |
| 3 | **49.45%** | **0.367** |
| 4 | **79.05%** | **0.005** |

R²(V) = 0.174（前 4 个主成分基本不可用）

#### 5.2.3 Full Reorthogonalization (FRO) 解决方案

每一步 j ≥ 2 时，强制让 W_new 减去它在 V_0..V_{j-1} 所有历史向量上的投影：

\[
W_{new} \leftarrow W_{new} - \sum_{k=0}^{j-1} (V_k \cdot W_{new}) \cdot V_k
\]

这是经典的 Gram-Schmidt 重正交化，把累积的"非正交污染"在每一步都清零。

代码：

```208:218:src/server.cpp
            // Step 3b (Optional): Full Reorthogonalization (HE-FRO)
            if (enable_fro && iter >= fro_skip) {
                for (int k = 0; k < iter; ++k) {
                    const auto& Vk = out.V_all[k];
                    auto proj_k = innerProduct(Vk, Wnew, d);
                    auto sub_k = scalarVecMultiply(proj_k, Vk);
                    Wnew = cc_->EvalSub(Wnew, sub_k);
                }
            }
```

打开 FRO 后同一组数据：

| i | λᵢ 误差 | cos_simᵢ |
|---|---|---|
| 1 | 2.86% | 0.9995 |
| 2 | 2.02% | 0.999 |
| 3 | **0.07%** | **0.999** |
| 4 | 1.61% | 0.994 |

R²(V) = **0.996**。

#### 5.2.4 FRO 的代价

| 代价 | 量化 |
|---|---|
| 单步深度增量 | +2 层 × (j − fro_skip) ≈ 最坏 +10 层 |
| 单步操作数增量 | 最坏 +14 mults + 98 rotates（j=7 时） |
| 总耗时增量 | d=50 时 +47%（133s → 196s）|

我们默认 `fro_skip_first=2` 跳过 j=0,1 步——前两步数学上可证 V₀ ⊥ V₁，不会有正交性损失，省 4 层深度。

### 5.3 Newton 1/√x 怎么在密文里做

#### 5.3.1 标准 Newton-Raphson 公式

求 1/√x 等价于解 f(y) = 1/y² − x = 0：
\[
y_{k+1} = \frac{y_k(3 - x y_k^2)}{2}
\]

代码：

```43:46:src/newton_inv_sqrt.h
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> newtonStep(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& x_ct,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& y,
        double half_factor) const;
```

每一步深度：

| 子操作 | 深度 |
|---|---|
| y² | 1 层（EvalMult(y, y)）|
| x · y² | 1 层 |
| (3 − x·y²) → ·y → ·0.5 | 1 层 |
| **每步合计** | **3 层** |

n 步 Newton 总深度 ≈ 3n + 1 层。我们用 newton_iters=2 或 3。

#### 5.3.2 收敛条件 + GUESS_SAFETY

Newton 单调收敛要求初值 y₀ 满足：
\[
z_0 = y_0 \sqrt{x} < \sqrt{3} \approx 1.732
\]

如果 z₀ > √3 会发生 "overshoot"，y_k 在 0 附近震荡甚至发散。

我们用一个保守初值：先在客户端**明文**算一遍 Lanczos（plaintext mirror）得到 ‖W‖² 真值估计 g，然后取
\[
y_0 = \frac{1}{\sqrt{\text{safety} \cdot g}}
\]

那么 z₀ = √(1/safety)：

| safety | z₀ | 离 √3 余量 | 风险 |
|---|---|---|---|
| 1 | 1.000 | 0.73 | 边界 |
| 2 | 0.707 | 1.02 | 紧 |
| **4（推荐）** | **0.500** | 1.23 | 安全 |
| 8 | 0.354 | 1.38 | 保守，收敛慢 |

代码：

```74:83:src/main.cpp
static double defaultGuessSafety(int d)
{
    if (d <= 32) return 1.0;
    if (d <= 128) return 2.0;
    if (d <= 256) return 4.0;
    if (d <= 784) return 8.0;
    return 16.0;
}
```

在 d=784 实测时，把默认 safety=8 调到 4 + newton_iters 从 2 升到 3，让 cos₁ 从 0.83 一步跳到 0.9998（[§7.1](#71-为什么-d784-默认-newton2-不够))。

### 5.4 Bootstrap 何时触发

代码：

```157:160:src/server.cpp
    const uint32_t per_iter_depth = static_cast<uint32_t>(
        4 + 4 * std::max(newton_steps_used, 1) + 2 * max_reorth_steps);
    const uint32_t bootstrap_threshold = per_iter_depth + 2;
```

进入 Lanczos 第 j 步前，如果 V/V_prev/β_prev 当前剩余深度 < bootstrap_threshold，先 Bootstrap 一次。

公式 `4 + 4·newton_steps + 2·max_reorth_steps` 来自 [§3.6](#36-lanczos-三项递推核心循环) 和 [§5.2](#52-当-m4lanczos-失去正交性fro-介入)：

| 子步 | 深度 |
|---|---|
| matvec + α 计算 + β·V_prev 减法 | 4 层 |
| Newton 每步 | 3 层 |
| FRO 每个 reorth 步 | 2 层（在 W_new 上累计）|

m=8, newton=2, FRO 启用、跳过 2 步：max_reorth_steps = m−1−2 = 5 → per_iter_depth = 4 + 8 + 10 = 22 层。

实测 d=50, m=8, FRO=ON：BS 总共 16 次（m=8 步 × 平均 2 次 BS = 16 次合理），每次 ~3.7s，总计 ~60s。

### 5.5 实测耗时分布（d=784, m=8, newton=3, FRO=ON）

| 算子（8 步累计） | 数量 | 耗时 | 占总 server 比例 |
|---|---|---|---|
| **matvec inner 的 EvalRotate** | 87 808 | **4 830 s** | **68 %** |
| matvec inner 的 EvalMult | 6 272 | 1 003 s | 14 % |
| matvec packScalars 的 mask EvalMult | 6 272 | 1 003 s | 14 % |
| FRO 的 mult+rotate | ~600 op | ~50 s | <1 % |
| Newton (3 步×8) | 72 mult | 12 s | <1 % |
| α/β/norm_sq inner | ~360 op | ~22 s | <1 % |
| **Bootstrap × 16** | — | **194 s** | **2.7 %** |
| **总计** | | **~7 124 s** | 100 % |

**单一最大瓶颈是 matvec 内部的 d × log₂(numSlots) ≈ 11 000 次 EvalRotate**，占整个 server 时间约 68%。Bootstrap 反而只占 2.7%。

---

## 6. 代码导航地图

### 6.1 文件结构

```
src/
├── client.h / client.cpp         ← Client 端：CKKS 上下文、加解密、CW 滤波、特征向量重构
├── server.h / server.cpp         ← Server 端：matvec、innerProduct、Lanczos 主循环、FRO
├── newton_inv_sqrt.h/.cpp        ← Newton 1/√x 密态实现（标准 + aSOR 变体）
├── asor.h                        ← aSOR 加速 Newton 的相关因子（暂未默认启用）
├── pca_eval.h                    ← R²(X)、R²(V)、cosineSimilarity 等明文评估指标
├── main.cpp                      ← 端到端流水线、参数解析、环境变量、运行入口
├── plaintext_lanczos_tune.cpp    ← 独立工具：明文 Lanczos 做 PRO 扫描，验证 FRO 思路
└── depth_probe.cpp               ← 独立工具：实测各 OpenFHE 算子的乘法深度
```

### 6.2 关键函数 ↔ 走读章节

| 函数 | 章节 |
|---|---|
| `Client::generateLowRankDataset` | [§4.2](#42-client-第一步构造-c-的明文) |
| `Client::normalizeCovariance` | [§4.3](#43-归一化c--c--tracec) |
| `Client::encryptCovMatrix` / `encryptColumnVector` | [§4.4](#44-加密) |
| `Server::lanczosIteration`（主循环） | [§4.5](#45-server-端-lanczos-第-0-步-j0)–[§4.6](#46-server-端-lanczos-第-1-步-j1) |
| `Server::innerProduct`（rotate-and-sum） | [§4.5.2](#452-innerproduct-内部到底干了什么) |
| `Server::matmul` / `packScalarsToVector` | [§4.5.1](#451-matmulw--ĉ--v) |
| `NewtonInvSqrt::compute` | [§5.3](#53-newton-1√x-怎么在密文里做) |
| `Server::bootstrapIfNeeded` | [§5.4](#54-bootstrap-何时触发) |
| FRO 段（`server.cpp::lanczosIteration` 内 `if (enable_fro && iter >= fro_skip)`） | [§5.2.3](#523-full-reorthogonalization-fro-解决方案) |
| `Client::buildTridiagonalFromEncrypted` | [§4.7](#47-client-解密-αβ) |
| `Client::cullumWilloughbyFilter` | [§4.9](#49-cw-滤波) |
| `Client::decryptLanczosVectors` + QR | [§4.10](#410-解密-v_all--qr-重正交) |
| `Client::reconstructEigenvectors` | [§4.11](#411-主成分重构u_k--q--u_t) |

### 6.3 关键参数与环境变量

| 环境变量 | 默认值 | 含义 | 推荐配置 |
|---|---|---|---|
| `DATASET_N` | 0（用随机协方差） | 低秩样本数 | d×4 倍以上 |
| `TRUE_RANK` | min(d, 5) | 真实低秩结构的秩 | 5–50 |
| `NOISE_SIGMA` | 0.1 | 加性高斯噪声 σ | 0.5–2.0 |
| `K` | 3 | 提取前 K 个主成分 | 3–4 |
| `NEWTON_ITERS` | 2 | Newton 1/√x 步数 | d ≤ 256 → 2；d ≥ 512 → 3 |
| `GUESS_SAFETY` | 按 d 自动 | Newton 初值安全因子 | 4 是常用 sweet spot |
| `PER_ITER_GUESS` | 1 | 每步用明文 mirror 算 guess | 始终开 |
| `ENABLE_FRO` | 1 | 全重正交化 | 始终开 |
| `FRO_SKIP_FIRST` | 2 | 跳过前 N 步的 reorth | 默认即可 |
| `LEVELS_AFTER_BOOTSTRAP` | 自动算 | BS 后可用层数 | 不要手动改 |

代码入口（`main.cpp::main`）：

```327:362:src/main.cpp
    if (mode == "bootstrap") {
        // 默认 m_iter 提升到 8 以让 HE-FRO 真正发挥提取多个特征的优势
        int m_iter = (argc >= 3) ? std::atoi(argv[2]) : 8;
        int d      = (argc >= 4) ? std::atoi(argv[3]) : 10;
        ...
        // FRO 默认开启（保留环境变量 ENABLE_FRO=0 可关闭）
        bool enable_fro = envInt("ENABLE_FRO", 1) != 0;
        int fro_skip_first = envInt("FRO_SKIP_FIRST", 2);
        ...
```

运行命令示例：

```bash
# d=50 推荐配置（约 200s）
DATASET_N=500 TRUE_RANK=20 NOISE_SIGMA=2.0 K=4 \
ENABLE_FRO=1 NEWTON_ITERS=2 \
./build/he_pca bootstrap 8 50

# d=784 高精度配置（约 2 hr）
DATASET_N=2000 TRUE_RANK=50 NOISE_SIGMA=2.0 K=4 \
ENABLE_FRO=1 NEWTON_ITERS=3 GUESS_SAFETY=4 \
./build/he_pca bootstrap 8 784
```

---

## 7. FAQ 常见疑问

### 7.1 为什么 d=784 默认 newton=2 不够

trace(C) 在大维度下很大（d=784 时 trace ≈ 3332），归一化后 λ ≈ 真值/3332 落到 10⁻² 区域。Newton 在 x ~ 10⁻⁶ 这种小值区每步只把误差减半（线性收敛区），2 步只够把 5% 误差砍到 1.25%——但归一化后特征值本身就 0.03 级别，1.25% 相对误差就有 4×10⁻⁴ 绝对，让 β = √(‖W‖²) 的精度只有 ~3 位有效数字，传到 V_next 之后被进一步放大。

实测 d=784 newton=2 时 λ_HE 系统性低估约 50%（只拿到真值的一半）；newton=3 直接把这个偏差砍到 < 4%。

### 7.2 为什么 v₀ 用随机向量而不是某个固定向量

Krylov subspace = span{v₀, Ĉv₀, ..., Ĉ^{m−1}v₀}。Lanczos 只能在这个子空间里找特征向量。

如果 v₀ 在某个真特征向量 v_k 上的投影是 0（或极小），Lanczos 拿不到对应的 λ_k——[§4.14](#414-一个有意思的现象v_3-v_4-没拿到) 演示了这个边界情况。

随机单位向量在每个特征向量上的期望投影是 1/√d，足够 Lanczos 收敛到全部前 K 个真特征对。

### 7.3 为什么不直接用 SelfAdjointEigenSolver 解 C

那是明文方案，需要 Server 看到 C 的明文。我们的目标是 Server **看不到 C**——所以必须用 HE 算子重写整个 PCA 流水线。

### 7.4 CW 滤波是不是必须的？什么时候鬼影会出现

m 较小（m ≤ 5）+ 数据干净时，T 的全谱和子矩阵谱很少撞上，CW 几乎是 no-op。

但 m=8 + 噪声 σ ≥ 1 时，鬼影出现概率 5–20%（实测）。CW 是 m×m 的明文操作（开销可忽略），始终开是兜底。

### 7.5 RingDim 为什么是 2¹⁵ 而不是 2¹⁴

| RingDim | numSlots | 支持的最大 multiplicativeDepth | d 上限 |
|---|---|---|---|
| 2¹⁴ = 16384 | 8192 | ~36–40（HEStd_128） | 8192 |
| **2¹⁵ = 32768** | 16384 | ~70（HEStd_128） | 16384 |
| 2¹⁶ = 65536 | 32768 | ~150 | 太大 |

我们 multiplicativeDepth 已经到 48–52，必须 RingDim ≥ 2¹⁵。代价是单密文体积变 2 倍。这是 HE-PCA 的核心 trade-off：要深电路就要大 ring。

### 7.6 Bootstrap 占总时间这么少？感觉听说 BS 很贵

确实贵——单次 BS ~3.7s 在 d=50 总计 60s 跑下来还能占 30%。但当 d 上升到 784 时，**matvec 因为 d 倍 inner product**变得更贵，从单步 ~5s 暴涨到 ~890s。BS 单次时间几乎只跟 RingDim/multiplicativeDepth 走、跟 d 无关，所以 BS 占比反而下降。

---

## 8. 附录：从 main.cpp 看到的端到端日志（d=50, m=8, K=4, FRO=ON）

```
Lanczos 单步最大深度估算 = 22  → levels_after_bootstrap = 26
=== FHE-PCA [bootstrap] (d=50, m_iter=8, Newton=2, FRO=ON(skip_first=2)) ===

[Phase 1] Client setup
  数据: 低秩合成集 N=500 rank=20 noise=2
  CryptoContext + KeyGen 耗时: 4171 ms
  multiplicativeDepth = 48   numSlots = 16384
  trace(C) = 396.66
  plaintext sim: α₀=0.0358  ‖W‖²=5.22e-03  guess(safety=2)=1/√x=9.78
  per-iter Newton guess: ON  count=7  range=[1.98e-04, 1.04e-02]

[Phase 2] Server HE-Lanczos
  耗时: 195659 ms
  Bootstrap 次数: 16   累计耗时: 59694 ms
  α: 8  β: 7  V_all: 8

[Phase 3] Client post-processing
  T_8 = | ... 8×8 三对角矩阵 ... |
  CW 保留特征值: 4

============================================================
  结果验证 (反归一化: ×396.66)
============================================================

  真实特征值:  λ_1=103.35  λ_2=54.52  λ_3=27.15  λ_4=16.87

  #1  HE: 100.39  真实: 103.35  误差: 2.86%  cos_sim=0.9995
  #2  HE: 55.62   真实: 54.52   误差: 2.02%  cos_sim=0.9990
  #3  HE: 27.17   真实: 27.15   误差: 0.07%  cos_sim=0.9993
  #4  HE: 16.59   真实: 16.87   误差: 1.61%  cos_sim=0.9936

  R²(V) 主成分相似度 = 0.9957
  R²(X) 重建评分（密文 K=4）: 0.5086
  R²(X) 重建评分（明文 K=4）: 0.5090
  R²(X) 差距 = 0.0003  （≈ 与明文无差）
```

---

如果你想边读边跑实验，可以先用 `./build/he_pca bootstrap 2 10` 启动一个最小配置（约 5 秒），看完整 log 跟着 [§4](#4-完整走例d4-全程手算) 的步骤把每个数值对一遍。

如果还有不清楚的点，欢迎追问哪一节、哪一步。
