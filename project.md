# 基于云边协同的 HE-Lanczos PCA 原型验证方案 (PoC)

## 1. 核心设计原则
1. **规避密态非线性灾难**：在服务端（Server）只做乘法和加法；所有的除法、开方（归一化）和特征分解，均通过一次通信交由客户端（Client）在明文下瞬间完成。
2. **块并行化（Block Lanczos）**：引入块级操作（Block Size $= p$），充分利用 CKKS 的 SIMD 特性，极大地缩减迭代次数。
3. **消除正交性崩塌**：每次传回客户端时，在明文下执行一次完全正交化，确保得到的 Lanczos 基向量绝对精确。

## 2. 角色与数据定义
*   **Client（客户端）**：持有公钥 $pk$ 与私钥 $sk$。负责数据的加解密、求倒数、归一化、以及最后的小矩阵特征分解。
*   **Server（云端）**：持有公钥 $pk$ 和计算密钥（RelinKeys, GaloisKeys）。负责沉重的密文矩阵乘法（$O(d^2 \cdot p)$ 复杂度）。
*   **参数设定**：
    *   $d$：数据维度（如 100）。
    *   $p$：Block 大小（即并行提取的特征数，如 $p=5$）。
    *   $m$：Lanczos 迭代次数（通常 $m=2$ 或 $3$ 即可满足 PoC 需求）。
    *   $C_{d \times d}$：数据的协方差矩阵（客户端在明文计算好后加密上传）。

## 3. 完整算法步骤（按执行顺序）

### 阶段一：初始化（Client）
1.  **数据预处理与加密**：
    *   Client 在明文下计算协方差矩阵 $C = X^T X$。
    *   对 $C$ 进行同态加密，生成密态矩阵 $[[C]]$。*(为方便 PoC 验证，可用 1D 数组+旋转来模拟，或直接加密 $d \times d$ 个元素。)*
2.  **生成初始块向量**：
    *   生成一个 $d \times p$ 的随机矩阵 $V_1$（表示 $p$ 个并行的初始列向量）。
    *   在明文下对其进行正交归一化（QR 分解：$V_1, R = \text{qr}(V_1)$）。
    *   加密生成 $[[V_1]]$。
    *   令 $[[V_0]] = \mathbf{0}_{d \times p}$，$[[\mathbf{B}_0]] = \mathbf{0}_{p \times p}$。
3.  **发送给 Server**：发送 $[[C]], [[V_1]], [[V_0]], [[\mathbf{B}_0]]$。

---

### 阶段二：并行块 Lanczos 迭代循环 ($j = 1 \text{ to } m$)
*提示：这一步是核心协同过程，包含 Server 算力和 Client 矫正。*

#### Step 2.1: 密态块投影（Server 执行）
利用加密的协方差矩阵，对当前的块向量进行特征空间拉伸，并减去上一步的残余。
$$ [[W_j]] = [[C]] \otimes [[V_j]] - [[V_{j-1}]] \otimes [[\mathbf{B}_{j-1}]] $$
*(PoC 实现提示：这里全是密文与密文的乘法+加法，Server 会消耗 1 层乘法深度。)*

#### Step 2.2: 密态块内积计算（Server 执行）
计算 $p \times p$ 的投影系数矩阵 $[[\mathbf{A}_j]]$。
$$ [[\mathbf{A}_j]] = [[V_j]]^T \otimes [[W_j]] $$

#### Step 2.3: 密态局部正交化（Server 执行）
将 $W_j$ 从当前方向中剥离。
$$ [[W_{j\_new}]] = [[W_j]] - [[V_j]] \otimes [[\mathbf{A}_j]] $$
*(完成后，Server 将仅有 2 层深度的 $[[W_{j\_new}]]$ 和 $[[\mathbf{A}_j]]$ 发送回 Client。)*

#### Step 2.4: 明文解密、完全正交与归一化（Client 介入）
1.  **解密**：Client 使用 $sk$ 解密，得到明文的小矩阵 $\mathbf{A}_j$ 和残差块矩阵 $W_{j\_new}$。
2.  **强制完全正交化（物理纠偏）**：
    为了消除 HE 乘法带来的近似误差，Client 强行将 $W_{j\_new}$ 与**所有**历史保存的明文基块 $V_1, V_2, \dots, V_j$ 正交：
    $$ \hat{W}_{j\_new} = W_{j\_new} - \sum_{i=1}^{j} V_i (V_i^T W_{j\_new}) $$
3.  **QR 分解提取新基底与 $\mathbf{B}_j$（解决开方与除法难题）**：
    对修正后的残差块进行明文的 QR 分解：
    $$ V_{j+1}, \mathbf{B}_j = \text{QR}(\hat{W}_{j\_new}) $$
    *(解释：这里的 $V_{j+1}$ 就是归一化后的正交基，$\mathbf{B}_j$ 就是替代了标准 Lanczos 中标量 $\beta_j$ 的上三角矩阵！)*
4.  **状态更新与发回**：
    Client 记录下 $\mathbf{A}_j, \mathbf{B}_j, V_{j+1}$。然后将新鲜的、乘法深度被重置为 0 的 $[[V_{j+1}]]$ 和 $[[\mathbf{B}_j]]$ 加密发给 Server，开启下一轮迭代。

---

### 阶段三：主成分的特征提取与映射（收尾阶段）

完成 $m$ 轮迭代后，Client 已经收集了 $m$ 个 $\mathbf{A}_j$ 和 $m-1$ 个 $\mathbf{B}_j$。
同时，Server 手中握有密态的基矩阵 $[[V_{total}]] = [ [[V_1]], [[V_2]], \dots, [[V_m]] ]$（尺寸为 $d \times (m \cdot p)$）。

1.  **Client 构建块三对角矩阵**：
    Client 在明文中拼装出一个大小为 $(m \cdot p) \times (m \cdot p)$ 的带状分块对称矩阵 $T_m$：
    对角线块为 $\mathbf{A}_1, \dots, \mathbf{A}_m$；次对角线块为 $\mathbf{B}_1^T, \dots, \mathbf{B}_{m-1}^T$。
2.  **Client 求解极小矩阵特征值（微秒级）**：
    Client 在明文中对 $T_m$ 进行标准特征分解（`np.linalg.eigh`），提取出最大的 $K$ 个特征值，以及对应的特征向量矩阵 $U_K$（尺寸为 $(m \cdot p) \times K$）。
3.  **生成主成分（Server 最终映射）**：
    Client 将 $U_K$ 发送给 Server（这只是一个明文的小矩阵，无需加密）。
    Server 进行最后一次密态-明文矩阵乘法：
    $$ [[P_K]] = [[V_{total}]] \otimes U_K $$
    **$[[P_K]]$ 就是最终求得的 $K$ 个 PCA 主成分向量的密文表示！**

---


请基实现上述的混合型 Block Lanczos PCA 原型。

### 编码要求
1. **环境与依赖**：C++17 标准，请使用 `<seal/seal.h>` 和 `<Eigen/Dense>`。
2. **CKKS 参数设置**：请在开头配置一个合理的 CKKS 参数（如 `poly_modulus_degree = 8192`，使用 40-bit 的 primes），并在 Client 构造函数中生成并分享 Context 和 Keys。
3. **架构清晰**：严格区分 `class Client` 和 `class Server`。Server 的方法签名中，参数类型只能是 `seal::Ciphertext` 或 `seal::Plaintext`。
4. **可编译性优先**：如果密态的 `Matrix-Vector Multiplication` 逻辑在 SEAL 中用 SIMD 实现太繁琐，允许在 PoC 中使用朴素的密文乘密文和 `evaluator.add` 循环累加来实现点乘（用可读性换性能，先把全流程跑通）。
5. 在代码中保留详尽的中文注释，解释你的数据打包方式。