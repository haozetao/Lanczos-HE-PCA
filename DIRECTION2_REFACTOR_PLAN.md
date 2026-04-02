# 方向2改造方案：纯 Server 无重正交 Block Lanczos + Cullum-Willoughby 过滤

## 0. 改造目标

**将当前"每轮回传 Client 做正交化"的方案改造为"Server 全程密态计算 + Client 仅做最终后处理"的纯 Server 方案。**

### 改造前 vs 改造后

| 维度 | 当前方案 (REPORT.md) | **改造后（方向2）** |
|------|---------------------|-------------------|
| Client-Server 交互轮次 | **m 轮**（每轮迭代都回传） | **2 轮**（初始 + 取结果） |
| Server 职责 | 仅做 C·V 矩阵乘法 | **完整 Lanczos 迭代**（矩阵乘 + 内积 + 局部正交 + 归一化） |
| Client 职责 | 解密→正交化→QR→重加密 | 初始加密 + **最终解密→CW 过滤→特征分解** |
| 密文 1/√x | 不需要 | **需要（Newton 迭代法）** |
| 重正交化 | Client 明文做 FRO | **不做**（CW 过滤去鬼影） |
| CKKS 乘法深度 | 2 层/轮（极浅，re-encrypt 重置） | **~22 层总计**（需要更大参数） |

---

## 1. 改造涉及的文件与改动总览

```
PCA/src/
├── main.cpp           // 【重写】三阶段流程 → 两阶段流程
├── client.h/cpp       // 【大改】删除每轮正交化，新增 CW 过滤 + 初始化改造
├── server.h/cpp       // 【大改】新增完整 Lanczos 迭代、Newton 1/√x、归一化
└── newton_inv_sqrt.h/cpp  // 【新增】密文 1/√x 逼近模块
```

### 各文件改动清单

| 文件 | 改动类型 | 具体内容 |
|------|---------|---------|
| `server.h` | **大改** | 新增 `lanczosIteration()`, `innerProductVec()`, `scalarVecMultiply()`, `broadcastScalar()`, `newtonInvSqrt()` 方法 |
| `server.cpp` | **大改** | 实现完整的 Lanczos 迭代循环（含 1/√x）|
| `client.h` | **大改** | 删除 `fullReorthogonalize()`, `qrDecompose()`；新增 `cullumWilloughbyFilter()`, `buildTridiagonal()` |
| `client.cpp` | **大改** | 删除每轮校正逻辑；新增 CW 过滤 + 标准三对角特征分解 |
| `main.cpp` | **重写** | 从三阶段（初始化→m轮迭代→提取）改为两阶段（初始化+发送→Server计算→Client 接收+后处理）|
| `newton_inv_sqrt.h/cpp` | **新增** | Newton 迭代法计算密文 1/√x |

---

## 2. CKKS 参数改造

### 2.1 为什么需要改参数

当前方案每轮只消耗 2 层深度就回传 Client 重加密，所以 `{60, 40, 40, 60}` 共 2 层就够了。

改造后 Server 要一次性跑完所有 Lanczos 步，深度消耗大幅增加：

```
每步 Lanczos 的深度消耗：
  Step 1: C·V 矩阵乘             → 1 层（密文×密文 + rescale）
  Step 2: V^T·W 内积              → 1 层
  Step 3: W - V·A 局部正交        → 1 层（标量-向量乘）
  Step 4: w^T·w 内积              → 1 层
  Step 5: Newton 1/√x (d_newton 次) → 3 × d_newton 层
  Step 6: w · (1/√x) 归一化       → 1 层
  ─────────────────────────────
  单步总计 ≈ 5 + 3 × d_newton 层

  d_newton = 5 时：单步 ≈ 20 层
  m = 5 步：总计 ≈ 100 层（需要 Bootstrapping）
  m = 3 步：总计 ≈ 60 层（仍需 Bootstrapping）
```

### 2.2 新参数配置

**方案 A：不使用 Bootstrapping（限制步数，初期推荐）**

```cpp
// 新参数：增大 poly_modulus_degree 和 coeff_modulus 链
seal::EncryptionParameters params(seal::scheme_type::ckks);
params.set_poly_modulus_degree(32768);  // 从 8192 → 32768

// coeff_modulus 链：提供 ~25 层乘法深度
// 首尾各一个特殊 prime，中间 25 个工作 prime
std::vector<int> bit_sizes = {60};        // 首 prime
for (int i = 0; i < 25; ++i)
    bit_sizes.push_back(40);              // 25 个工作 prime
bit_sizes.push_back(60);                  // 末 prime (special prime)

params.set_coeff_modulus(seal::CoeffModulus::Create(32768, bit_sizes));
double scale = pow(2.0, 40);
```

| 参数 | 旧值 | **新值** | 说明 |
|------|------|---------|------|
| `poly_modulus_degree` | 8192 | **32768** | 支持更深的乘法链 + 更多 slots |
| `coeff_modulus` | {60,40,40,60} = 2层 | **{60, 40×25, 60} = 25层** | 足够 m=1 步不带 Bootstrap |
| `slot_count` | 4096 | **16384** | 远大于 d=10 |
| GaloisKeys 旋转步长 | {1,2,4,8} | **{1,2,4,8,16}** | 增加 16 以支持更大的 rotate-and-sum |

> **注意**：25 层深度只够 m=1 步完整 Lanczos（含 Newton 5 次迭代）。
> 要跑 m=3 步，需要使用 Bootstrapping（方案 B）或增大到 poly_mod=65536。
> **初期建议先用 m=1 验证 Newton 1/√x 的正确性，再逐步增加 m。**

**方案 B：使用 Bootstrapping（后续阶段）**

```cpp
params.set_poly_modulus_degree(65536);  // Bootstrapping 需要更大环
// Bootstrap 可在深度耗尽时刷新，理论上支持无限步数
// 但单次 Bootstrap 耗时 ~10-60 秒
```

---

## 3. 新增模块：密文 Newton 1/√x

### 3.1 文件：`newton_inv_sqrt.h`

```cpp
#pragma once
#include <seal/seal.h>
#include <vector>

// Newton 迭代法在 CKKS 密文上计算 1/√x
//
// 迭代公式：y_{n+1} = y_n · (3 - x · y_n²) / 2
//
// 深度消耗：每次迭代 3 层（y², x·y², y·(...)）
//           d 次迭代总计 3d 层
//
// 输入约束：x 应在 [0.1, 100] 范围内（归一化后）
//          初始猜测 y_0 应接近 1/√x（用明文估计值）

struct InvSqrtResult {
    seal::Ciphertext inv_sqrt;  // 密文 1/√x
    seal::Ciphertext sqrt_val;  // 密文 √x = x · (1/√x)
};

class NewtonInvSqrt {
public:
    NewtonInvSqrt(
        std::shared_ptr<seal::SEALContext> context,
        seal::Evaluator& evaluator,
        seal::CKKSEncoder& encoder,
        seal::Encryptor& encryptor,
        const seal::RelinKeys& relin_keys,
        double scale
    );

    // 核心方法：密文 1/√x
    // x_ct: 标量密文（slot[0] 存储 x 值）
    // initial_guess: 明文初始猜测 ≈ 1/√x
    // iterations: Newton 迭代次数（推荐 5~7）
    InvSqrtResult compute(
        const seal::Ciphertext& x_ct,
        double initial_guess,
        int iterations
    ) const;

private:
    std::shared_ptr<seal::SEALContext> context_;
    seal::Evaluator& evaluator_;
    seal::CKKSEncoder& encoder_;
    seal::Encryptor& encryptor_;
    const seal::RelinKeys& relin_keys_;
    double scale_;

    // 辅助：对齐两个密文的 level 和 scale
    void matchLevels(seal::Ciphertext& a, seal::Ciphertext& b) const;
};
```

### 3.2 文件：`newton_inv_sqrt.cpp`

```cpp
#include "newton_inv_sqrt.h"
#include <cmath>

NewtonInvSqrt::NewtonInvSqrt(
    std::shared_ptr<seal::SEALContext> context,
    seal::Evaluator& evaluator,
    seal::CKKSEncoder& encoder,
    seal::Encryptor& encryptor,
    const seal::RelinKeys& relin_keys,
    double scale)
    : context_(context), evaluator_(evaluator), encoder_(encoder),
      encryptor_(encryptor), relin_keys_(relin_keys), scale_(scale) {}

void NewtonInvSqrt::matchLevels(seal::Ciphertext& a, seal::Ciphertext& b) const {
    size_t lvl_a = context_->get_context_data(a.parms_id())->chain_index();
    size_t lvl_b = context_->get_context_data(b.parms_id())->chain_index();
    if (lvl_a > lvl_b)
        evaluator_.mod_switch_to_inplace(a, b.parms_id());
    else if (lvl_b > lvl_a)
        evaluator_.mod_switch_to_inplace(b, a.parms_id());
    a.scale() = scale_;
    b.scale() = scale_;
}

InvSqrtResult NewtonInvSqrt::compute(
    const seal::Ciphertext& x_ct,
    double initial_guess,
    int iterations) const
{
    // y_0 = initial_guess (明文编码为密文)
    seal::Plaintext y0_pt;
    encoder_.encode(initial_guess, scale_, y0_pt);
    seal::Ciphertext y;
    encryptor_.encrypt(y0_pt, y);

    for (int i = 0; i < iterations; ++i) {
        // === y² ===
        seal::Ciphertext y_sq;
        evaluator_.square(y, y_sq);                      // 深度 +1
        evaluator_.relinearize_inplace(y_sq, relin_keys_);
        evaluator_.rescale_to_next_inplace(y_sq);

        // === x · y² ===
        seal::Ciphertext x_copy = x_ct;
        matchLevels(x_copy, y_sq);
        seal::Ciphertext x_y_sq;
        evaluator_.multiply(x_copy, y_sq, x_y_sq);      // 深度 +1
        evaluator_.relinearize_inplace(x_y_sq, relin_keys_);
        evaluator_.rescale_to_next_inplace(x_y_sq);

        // === 3 - x·y² ===
        seal::Plaintext three_pt;
        encoder_.encode(3.0, x_y_sq.scale(), three_pt);
        evaluator_.mod_switch_to_inplace(three_pt, x_y_sq.parms_id());
        seal::Ciphertext diff;
        evaluator_.negate(x_y_sq, diff);                 // -x·y²
        evaluator_.add_plain_inplace(diff, three_pt);    // 3 - x·y²

        // === y · (3 - x·y²) ===
        matchLevels(y, diff);
        seal::Ciphertext product;
        evaluator_.multiply(y, diff, product);           // 深度 +1
        evaluator_.relinearize_inplace(product, relin_keys_);
        evaluator_.rescale_to_next_inplace(product);

        // === 除以 2 (明文乘 0.5) ===
        seal::Plaintext half_pt;
        encoder_.encode(0.5, product.scale(), half_pt);
        evaluator_.mod_switch_to_inplace(half_pt, product.parms_id());
        evaluator_.multiply_plain_inplace(product, half_pt);
        evaluator_.rescale_to_next_inplace(product);

        y = product;
    }

    // 计算 √x = x · (1/√x)
    seal::Ciphertext x_copy = x_ct;
    matchLevels(x_copy, y);
    seal::Ciphertext sqrt_val;
    evaluator_.multiply(x_copy, y, sqrt_val);
    evaluator_.relinearize_inplace(sqrt_val, relin_keys_);
    evaluator_.rescale_to_next_inplace(sqrt_val);

    return InvSqrtResult{y, sqrt_val};
}
```

---

## 4. Server 改造：完整 Lanczos 迭代

### 4.1 新增方法列表

```cpp
// server.h 新增方法

class Server {
public:
    // 原有方法（保留）
    std::vector<seal::Ciphertext> matmul(
        const std::vector<seal::Ciphertext>& enc_C,
        const std::vector<seal::Ciphertext>& enc_V,
        int d, int p);

    // ===== 新增方法 =====

    // 核心：完整 Lanczos 迭代（Server 一次性跑完 m 步）
    struct LanczosResult {
        // 三对角矩阵元素（密文标量）
        std::vector<seal::Ciphertext> alphas;  // m 个 [[α_j]]
        std::vector<seal::Ciphertext> betas;   // m-1 个 [[β_j]]
        // 可选：所有基向量（用于后续投影）
        std::vector<std::vector<seal::Ciphertext>> V_all; // m 组，每组 p 个密文
    };

    LanczosResult lanczosIteration(
        const std::vector<seal::Ciphertext>& enc_C,  // d 个密文（行打包）
        const std::vector<seal::Ciphertext>& enc_V1, // p 个密文（列打包）
        int d, int p, int m_iter,
        double eigenvalue_estimate  // 用于 Newton 初始猜测
    );

    // 密文向量内积：两个列打包向量的内积 → 标量密文
    seal::Ciphertext innerProductVec(
        const seal::Ciphertext& a,
        const seal::Ciphertext& b,
        int dim);

    // 标量密文广播到所有 slot
    seal::Ciphertext broadcastScalar(
        const seal::Ciphertext& scalar_ct,
        int dim);

    // 标量密文 × 向量密文
    seal::Ciphertext scalarVecMultiply(
        const seal::Ciphertext& scalar_ct,
        const seal::Ciphertext& vec_ct,
        int dim);

private:
    // 原有 innerProduct 方法（保留，用于矩阵乘）
    seal::Ciphertext innerProduct(
        const seal::Ciphertext& row_ct,
        const seal::Ciphertext& col_ct);

    std::unique_ptr<NewtonInvSqrt> newton_; // 新增
};
```

### 4.2 `lanczosIteration()` 实现（核心改造）

```cpp
// server.cpp 中新增

Server::LanczosResult Server::lanczosIteration(
    const std::vector<seal::Ciphertext>& enc_C,
    const std::vector<seal::Ciphertext>& enc_V1,
    int d, int p, int m_iter,
    double eigenvalue_estimate)
{
    LanczosResult result;
    result.alphas.reserve(m_iter);
    result.betas.reserve(m_iter);
    result.V_all.reserve(m_iter + 1);

    // 初始化
    auto enc_V_curr = enc_V1;                // 当前基 V_j（p 个列密文）
    result.V_all.push_back(enc_V_curr);

    // V_prev = 零向量，beta_prev = 零标量
    std::vector<seal::Ciphertext> enc_V_prev(p);
    std::vector<seal::Ciphertext> enc_beta_prev(p);
    for (int j = 0; j < p; ++j) {
        seal::Plaintext zero_pt;
        encoder_.encode(std::vector<double>(slot_count_, 0.0), scale_, zero_pt);
        encryptor_.encrypt(zero_pt, enc_V_prev[j]);
        encryptor_.encrypt(zero_pt, enc_beta_prev[j]);
    }

    for (int iter = 0; iter < m_iter; ++iter) {
        // ============================================================
        // Step 1: 矩阵-向量乘法  W = C · V_curr
        //         (简化版：不减 V_prev·B_prev，与当前 PoC 一致)
        // ============================================================
        auto enc_W = matmul(enc_C, enc_V_curr, d, p);
        // enc_W 是 d*p 个标量密文
        // 需要重组为 p 个列向量密文（每列 d 个元素）
        // 重组方法：将 d 个标量密文编码到一个向量密文的对应 slot 中
        // （这里需要额外处理，见下方 §4.3）

        // ============================================================
        // Step 2: 计算 α = V_curr^T · W（内积，得到标量/小矩阵）
        //         对于 p=1 的简化情况：α 是一个标量密文
        //         对于 p=2 的 Block 情况：α 是 p×p 小矩阵
        // ============================================================
        // 简化版（p=1 或逐列处理）：
        for (int col = 0; col < p; ++col) {
            seal::Ciphertext alpha_col = innerProductVec(
                enc_V_curr[col], enc_W_cols[col], d);
            result.alphas.push_back(alpha_col);
        }

        // ============================================================
        // Step 3: 局部正交化  W_new = W - V_curr · α
        //         *** 不做完全重正交化（FRO），这是方向2的核心改动 ***
        // ============================================================
        std::vector<seal::Ciphertext> enc_W_new(p);
        for (int col = 0; col < p; ++col) {
            // α_col 广播后 × V_curr[col]
            seal::Ciphertext alpha_v = scalarVecMultiply(
                result.alphas.back(), enc_V_curr[col], d);
            // W_new[col] = W[col] - α·V[col]
            evaluator_.sub(enc_W_cols[col], alpha_v, enc_W_new[col]);
        }

        // ============================================================
        // Step 4+5: 归一化  β = ||W_new||, V_next = W_new / β
        //           使用 Newton 法计算密文 1/√x
        // ============================================================
        std::vector<seal::Ciphertext> enc_V_next(p);
        for (int col = 0; col < p; ++col) {
            // w^T · w → 标量密文
            seal::Ciphertext norm_sq = innerProductVec(
                enc_W_new[col], enc_W_new[col], d);

            // Newton 1/√x
            double guess = 1.0 / std::sqrt(eigenvalue_estimate);
            InvSqrtResult inv_sqrt = newton_->compute(norm_sq, guess, 5);

            // β = √x = x · (1/√x)  → 已在 newton 里算好
            result.betas.push_back(inv_sqrt.sqrt_val);

            // V_next = W_new · (1/√x) → 广播 1/√x 到所有 slot 后逐元素乘
            enc_V_next[col] = scalarVecMultiply(
                inv_sqrt.inv_sqrt, enc_W_new[col], d);
        }

        // 保存并准备下一轮
        enc_V_prev = enc_V_curr;
        enc_V_curr = enc_V_next;
        result.V_all.push_back(enc_V_curr);
    }

    return result;
}
```

### 4.3 矩阵乘结果重组

当前 `matmul()` 输出 d×p 个独立的标量密文。Lanczos 后续步骤需要**列向量密文**。需要新增一个重组函数：

```cpp
// 将 d 个标量密文（slot[0] 有值）打包成 1 个列向量密文（slot[0..d-1] 有值）
seal::Ciphertext Server::packScalarsToVector(
    const std::vector<seal::Ciphertext>& scalar_cts,  // d 个标量密文
    int d)
{
    // 方法：每个标量密文旋转到目标 slot 位置，然后全部加起来
    seal::Ciphertext result = scalar_cts[0];  // slot[0] 已经有第一个标量
    for (int i = 1; i < d; ++i) {
        // 将 scalar_cts[i] 的 slot[0] 旋转到 slot[i]
        seal::Ciphertext rotated;
        evaluator_.rotate_vector(scalar_cts[i], -i, galois_keys_, rotated);
        evaluator_.add_inplace(result, rotated);
    }
    return result;
}
```

### 4.4 新增辅助方法实现

```cpp
// 密文向量内积（两个列打包密文 → 标量密文 slot[0]）
seal::Ciphertext Server::innerProductVec(
    const seal::Ciphertext& a,
    const seal::Ciphertext& b,
    int dim)
{
    // Step 1: 逐元素乘
    seal::Ciphertext product;
    evaluator_.multiply(a, b, product);
    evaluator_.relinearize_inplace(product, relin_keys_);
    evaluator_.rescale_to_next_inplace(product);

    // Step 2: rotate-and-sum（与原 innerProduct 相同逻辑）
    for (int shift = 1; shift < dim; shift <<= 1) {
        seal::Ciphertext rotated;
        evaluator_.rotate_vector(product, shift, galois_keys_, rotated);
        evaluator_.add_inplace(product, rotated);
    }
    return product;  // slot[0] = Σ a[i]*b[i]
}

// 标量密文广播：slot[0] 复制到 slot[0..dim-1]
seal::Ciphertext Server::broadcastScalar(
    const seal::Ciphertext& scalar_ct,
    int dim)
{
    seal::Ciphertext result = scalar_ct;
    for (int shift = 1; shift < dim; shift <<= 1) {
        seal::Ciphertext rotated;
        evaluator_.rotate_vector(result, -shift, galois_keys_, rotated);
        evaluator_.add_inplace(result, rotated);
    }
    return result;
}

// 标量密文 × 向量密文
seal::Ciphertext Server::scalarVecMultiply(
    const seal::Ciphertext& scalar_ct,
    const seal::Ciphertext& vec_ct,
    int dim)
{
    seal::Ciphertext broadcasted = broadcastScalar(scalar_ct, dim);
    seal::Ciphertext result;
    matchLevels(broadcasted, vec_ct);  // 需要新增 matchLevels 到 Server
    evaluator_.multiply(broadcasted, vec_ct, result);
    evaluator_.relinearize_inplace(result, relin_keys_);
    evaluator_.rescale_to_next_inplace(result);
    return result;
}
```

---

## 5. Client 改造：删除每轮正交化，新增 CW 过滤

### 5.1 删除的方法

```cpp
// 从 client.h/cpp 中删除：
void fullReorthogonalize(...);  // 不再需要：Server 不回传中间结果
void qrDecompose(...);          // 不再需要：归一化在 Server 密文上做
// 删除每轮的 解密→校正→重加密 循环
```

### 5.2 新增：Cullum-Willoughby 过滤

```cpp
// client.h 新增

struct CWFilterResult {
    Eigen::VectorXd good_eigenvalues;    // 过滤后的真特征值
    Eigen::MatrixXd good_eigenvectors;   // 对应的特征向量
};

class Client {
public:
    // ... 原有方法（保留 encryptCovMatrix, encryptBlockVec, decryptToMatrix）

    // ===== 新增方法 =====

    // 从 Server 返回的密文 α, β 解密并构造三对角矩阵
    Eigen::MatrixXd buildTridiagonalFromEncrypted(
        const std::vector<seal::Ciphertext>& enc_alphas,
        const std::vector<seal::Ciphertext>& enc_betas,
        int m);

    // Cullum-Willoughby 过滤：识别并去除鬼影特征值
    CWFilterResult cullumWilloughbyFilter(
        const Eigen::MatrixXd& T_m,
        int K);
};
```

### 5.3 CW 过滤实现

```cpp
// client.cpp 新增

Eigen::MatrixXd Client::buildTridiagonalFromEncrypted(
    const std::vector<seal::Ciphertext>& enc_alphas,
    const std::vector<seal::Ciphertext>& enc_betas,
    int m)
{
    // 解密 α 和 β
    std::vector<double> alphas(m), betas(m - 1);
    for (int i = 0; i < m; ++i) {
        seal::Plaintext pt;
        decryptor_->decrypt(enc_alphas[i], pt);
        std::vector<double> vals;
        encoder_->decode(pt, vals);
        alphas[i] = vals[0];
    }
    for (int i = 0; i < m - 1; ++i) {
        seal::Plaintext pt;
        decryptor_->decrypt(enc_betas[i], pt);
        std::vector<double> vals;
        encoder_->decode(pt, vals);
        betas[i] = vals[0];
    }

    // 构造标准三对角矩阵（非块三对角）
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

CWFilterResult Client::cullumWilloughbyFilter(
    const Eigen::MatrixXd& T_m,
    int K)
{
    int m = T_m.rows();

    // Step 1: T_m 的全部特征值
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_full(T_m);
    Eigen::VectorXd evals_full = solver_full.eigenvalues();
    Eigen::MatrixXd evecs_full = solver_full.eigenvectors();

    // Step 2: 构造子矩阵 T_s（删除第 1 行第 1 列）
    Eigen::MatrixXd T_s = T_m.bottomRightCorner(m - 1, m - 1);

    // Step 3: T_s 的全部特征值
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_sub(T_s);
    Eigen::VectorXd evals_sub = solver_sub.eigenvalues();

    // Step 4: 比较，过滤鬼影
    // 如果 T_m 的某个特征值在 T_s 中也出现（距离 < tolerance），则为鬼影
    double tolerance = 1e-6 * T_m.norm();  // 相对容差
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

    // Step 5: 提取前 K 个最大的"好"特征值
    // 先按降序排列
    std::sort(good_indices.begin(), good_indices.end(),
              [&](int a, int b) { return evals_full(a) > evals_full(b); });

    int n_good = std::min(K, (int)good_indices.size());
    Eigen::VectorXd good_evals(n_good);
    Eigen::MatrixXd good_evecs(m, n_good);

    for (int i = 0; i < n_good; ++i) {
        good_evals(i) = evals_full(good_indices[i]);
        good_evecs.col(i) = evecs_full.col(good_indices[i]);
    }

    return CWFilterResult{good_evals, good_evecs};
}
```

---

## 6. main.cpp 改造

### 6.1 从三阶段改为两阶段

```cpp
// main.cpp 重写

int main() {
    // ======== 参数 ========
    const int d = 10;       // 特征维度
    const int p = 1;        // 初期简化：p=1（标准 Lanczos，非 Block）
    const int m_iter = 3;   // Lanczos 步数
    const int K = 2;        // 目标主成分数
    const int newton_iters = 5;  // Newton 1/√x 迭代次数

    std::cout << "=== HE-Lanczos PCA (纯 Server, 方向2) ===" << std::endl;

    // ======== Phase 1: Client 初始化 + 一次性发送 ========
    std::cout << "\n[Phase 1] Client 初始化..." << std::endl;

    Client client(/* 新 CKKS 参数 */);

    // 1.1 生成并加密协方差矩阵
    Eigen::MatrixXd C = generateCovMatrix(d);
    auto enc_C = client.encryptCovMatrix(C);

    // 1.2 生成随机单位向量，加密
    Eigen::VectorXd v1 = randomUnitVector(d);
    auto enc_v1 = client.encryptVector(v1);  // 单个列密文

    // 1.3 估计特征值范围（用 C 的 Frobenius 范数粗估）
    double eigenvalue_estimate = C.norm() / std::sqrt((double)d);

    // 1.4 创建 Server，一次性发送所有数据
    Server server(client.getContext(), client.getPublicKey(),
                  client.getRelinKeys(), client.getGaloisKeys());

    std::cout << "  已发送 enc_C (" << d << " 密文) + enc_v1 (1 密文) 给 Server"
              << std::endl;

    // ======== Phase 2: Server 一次性执行完整 Lanczos ========
    std::cout << "\n[Phase 2] Server 执行 Lanczos 迭代 (" << m_iter << " 步)..."
              << std::endl;

    auto lanczos_result = server.lanczosIteration(
        enc_C, {enc_v1}, d, p, m_iter, eigenvalue_estimate);

    std::cout << "  Server 完成，返回 " << lanczos_result.alphas.size()
              << " 个 α 密文 + " << lanczos_result.betas.size()
              << " 个 β 密文" << std::endl;

    // ======== Phase 3: Client 后处理 ========
    std::cout << "\n[Phase 3] Client 后处理..." << std::endl;

    // 3.1 解密并构造三对角矩阵
    Eigen::MatrixXd T_m = client.buildTridiagonalFromEncrypted(
        lanczos_result.alphas, lanczos_result.betas, m_iter);
    std::cout << "  三对角矩阵 T_" << m_iter << " 已构建" << std::endl;

    // 3.2 Cullum-Willoughby 过滤
    auto cw_result = client.cullumWilloughbyFilter(T_m, K);
    std::cout << "  CW 过滤完成：" << cw_result.good_eigenvalues.size()
              << " 个有效特征值" << std::endl;

    // 3.3 解密基向量 V_all，计算投影矩阵（可选）
    // ...

    // ======== 验证 ========
    std::cout << "\n========== 结果对比 ==========" << std::endl;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> true_solver(C);
    Eigen::VectorXd true_evals = true_solver.eigenvalues().reverse();

    for (int i = 0; i < K; ++i) {
        double he_val = cw_result.good_eigenvalues(i);
        double true_val = true_evals(i);
        double err = std::abs(he_val - true_val) / true_val * 100;
        std::cout << "  PC" << i+1
                  << " | HE Lanczos: " << he_val
                  << " | 真实值: " << true_val
                  << " | 误差: " << err << "%" << std::endl;
    }

    return 0;
}
```

---

## 7. 改造前后对比：信息流

### 改造前（当前方案）

```
Client                    Server
  │                         │
  │── enc_C, enc_V1 ──────→ │
  │                         │── matmul(enc_C, enc_V) ──→ enc_W
  │←── enc_W ──────────────│
  │  解密+正交化+QR+重加密   │
  │── enc_V2 ─────────────→│
  │                         │── matmul(enc_C, enc_V) ──→ enc_W
  │←── enc_W ──────────────│
  │  解密+正交化+QR+重加密   │
  │── enc_V3 ─────────────→│
  │        ...               │        ...
  │  (重复 m 次)              │
```

**交互：m 轮**

### 改造后（方向2）

```
Client                    Server
  │                         │
  │── enc_C, enc_V1 ──────→ │
  │                         │  ┌──────────────────────┐
  │                         │  │ for j = 1 to m:      │
  │                         │  │   W = C·V             │
  │                         │  │   α = V^T·W           │
  │                         │  │   W_new = W - α·V     │
  │                         │  │   β = Newton_1/√(w^Tw)│
  │                         │  │   V_next = w·(1/√x)   │
  │                         │  └──────────────────────┘
  │←── enc_α[], enc_β[] ──│
  │  CW过滤 + 特征分解      │
```

**交互：2 轮**

---

## 8. 分步实施建议

| 步骤 | 任务 | 验证方法 | 预计改动量 |
|------|------|---------|----------|
| **Step 0** | 修改 CKKS 参数（增大 poly_mod + 加深 coeff_modulus） | 加密→解密 round-trip 测试 | CMakeLists + client.cpp 参数部分 |
| **Step 1** | 实现 `newton_inv_sqrt.h/cpp` | 对 x=0.5,1,2,5,10 测试精度 < 1% | 新文件 ~120 行 |
| **Step 2** | 实现 `innerProductVec()`, `broadcastScalar()`, `scalarVecMultiply()` | 与明文内积结果对比 | server.cpp ~60 行 |
| **Step 3** | 实现 `packScalarsToVector()` | 打包→解密→比对 | server.cpp ~20 行 |
| **Step 4** | 实现 `lanczosIteration()` 主循环（先 p=1） | m=1 步：与明文 Lanczos 单步对比 | server.cpp ~100 行 |
| **Step 5** | 实现 Client 的 `buildTridiagonalFromEncrypted()` + `cullumWilloughbyFilter()` | 用明文 Lanczos 的 T_m 验证 CW | client.cpp ~80 行 |
| **Step 6** | 重写 `main.cpp` 为两阶段流程 | 端到端特征值对比 | main.cpp ~80 行 |
| **Step 7** | 扩展到 Block Lanczos (p=2) | 与 p=1 结果对比 | server.cpp 小改 |

**建议先做 Step 0 + Step 1 + Step 2，验证密文 1/√x 和基础向量运算的正确性后再推进后续步骤。**

---

## 9. 已知风险与应对

| 风险 | 影响 | 应对策略 |
|------|------|---------|
| Newton 1/√x 精度不足 | 归一化误差 → 特征值偏差 | 增加迭代次数 d；改用 Goldschmidt 算法；调整初始猜测 |
| CKKS 噪声累积 | m 步后密文噪声过大 | 减少 m；引入 Bootstrapping；增大 poly_mod |
| 深度不够 | m > 1 时深度耗尽 | 初期先验证 m=1；后续引入 Bootstrapping |
| 无重正交导致鬼影过多 | CW 过滤后有效特征值不足 K 个 | 增加 m_iter（多跑步数）；退回方向1（密文 FRO）|
| 广播旋转密钥过大 | GaloisKeys 内存占用 | 只生成需要的旋转步长 |