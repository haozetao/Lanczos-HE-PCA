#pragma once

#include <memory>
#include <vector>

#include <Eigen/Dense>
#include <openfhe.h>

// Cullum-Willoughby 过滤结果
struct CWFilterResult {
    Eigen::VectorXd good_eigenvalues;
    Eigen::MatrixXd good_eigenvectors;
};

// ============================================================================
// Client —— OpenFHE CKKS 版本（方向2：深 CKKS + 可选 Bootstrap）
//   - 构造时可选 enable_bootstrap=true 则启用 FHE 层并生成 Bootstrap 密钥
//   - 加解密使用 OpenFHE 的 CryptoContext API
//   - 所有纯明文后处理方法（CW、Gram-Schmidt 等）与 SEAL 版本保持一致
// ============================================================================
class Client {
public:
    // enable_bootstrap=false: 轻量模式，仅生成基本密钥，跳过 Bootstrap 密钥生成
    // levels_after_bootstrap: Bootstrap 后可用层数（预留给 Lanczos + Newton）
    // enable_he=false: 纯明文模式，跳过 CryptoContext / KeyGen，
    //   仅用于 plaintext mirror 对照与 ground-truth 计算。
    Client(int d = 10,
           int p = 1,
           int m = 3,
           bool enable_bootstrap = false,
           uint32_t levels_after_bootstrap = 12,
           bool enable_he = true);

    void generateCovarianceMatrix();

    // 生成低秩 + 噪声的合成数据集，用于 R²(X) 重建评估
    //   N         : 样本数
    //   true_rank : 真实低秩结构的秩（越小越利于 PCA 压缩）
    //   noise_sigma : 加性高斯噪声标准差（0 → 纯低秩）
    // 调用后内部保存：
    //   X_centered_ ∈ R^{N×d} （已减去均值）
    //   C_         = X_centered_^T · X_centered_ / (N-1)
    void generateLowRankDataset(int N, int true_rank, double noise_sigma);

    // 从二进制文件加载真实数据集（如 Yale / MNIST / Fashion-MNIST 16×16 灰度图），
    // 由 scripts/prepare_yale.py 等工具预处理生成。文件格式（little-endian）：
    //   uint32 N         # 样本数
    //   uint32 d_in      # 必须等于 d_
    //   float64 × N × d  # row-major X[N, d]，未中心化的原始像素值
    // 调用后内部保存：
    //   X_centered_     ← X − mean
    //   C_              ← X_centered_^T · X_centered_ / (N-1)
    // 若 normalize_to_unit=true，会先把 X 缩放到 [0, 1]（像素值 / 255），便于与
    // sklearn / Panda 2021 / Ma 2023 的 baseline 数值范围对齐。
    void generateFromBinaryFile(const std::string& path,
                                bool normalize_to_unit = true,
                                int max_samples = 0);

    // 已中心化的数据矩阵（仅在 generateLowRankDataset / generateFromBinaryFile 后有效）
    const Eigen::MatrixXd& centeredData() const { return X_centered_; }
    // 中心化时减去的样本均值 (1×d)，用来反中心化做重建可视化
    const Eigen::RowVectorXd& sampleMean() const { return sample_mean_; }
    bool hasData() const { return X_centered_.rows() > 0; }

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptCovMatrix() const;

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptBlockVec(
        const Eigen::MatrixXd& V) const;

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptColumnVector(
        const Eigen::VectorXd& v) const;

    // ── Hybrid (BSGS) Diagonal Packing ────────────────────────────────────────
    // Halevi-Shoup '18 / Ma 2023: 把 d×d 协方差矩阵按"循环对角线"打包，让
    //   (C·v)[i] = Σ_k diag_k[i] · v[(i+k) mod d]
    //       diag_k[i] = C[i, (i+k) mod d]
    // 主要好处：
    //   - matvec 内的 EvalRotate 数 d log d → d → 2√d（BSGS）
    //   - 不再需要 packScalarsToVector（直接输出 replicated 向量）
    //
    // 返回 d 个 ct。当 bsgs_b > 0 时：
    //   - d 必须能被 bsgs_b 整除，记 a = d / bsgs_b
    //   - 对 k = b*i + j（i ∈ [0,a), j ∈ [0,b)）的对角线，
    //     已在客户端预先 cyclic-rotate by -b*i，避免 server 端在每个 giant
    //     step 里多做一次密文旋转（用 plaintext 旋转省成本）
    // 当 bsgs_b == 0 时：按原始 diag_k 编码（无 BSGS）
    //
    // 所有对角线在 slot 内复制 (num_slots / d) 次，保证 Rot(v, k) 与 diag_k
    // 在长 ct 上做 element-wise mult 时仍然对齐。
    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptCovMatrixDiagonal(
        int bsgs_b = 0) const;

    // 把 v ∈ R^d 加密成单个 ct，其 slots 内复制 (num_slots / d) 次：
    //   slot[i] = v[i mod d]   for i in [0, num_slots)
    // 这是 Diagonal Packing 模式下所有 Lanczos 向量 (V, W, V_history) 的标准 layout。
    // 要求 num_slots % d == 0（实践中 d 取 2 的幂即可）。
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> encryptColumnVectorReplicated(
        const Eigen::VectorXd& v) const;

    // 把 replicated 向量 ct 解密为 R^d。仅取前 d 个 slot（其余是复制副本）。
    Eigen::VectorXd decryptReplicatedVector(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct) const;

    Eigen::MatrixXd decryptToMatrix(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_W,
        int rows,
        int cols) const;

    Eigen::MatrixXd buildTridiagonalFromEncrypted(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_alphas,
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_betas,
        int m) const;

    CWFilterResult cullumWilloughbyFilter(
        const Eigen::MatrixXd& T_m,
        int K) const;

    Eigen::MatrixXd plaintextMirrorHeLanczosTridiagonal(
        const Eigen::VectorXd& v0,
        int m_iter) const;

    // 明文 Lanczos + 可选的部分重正交化 (PRO) + CKKS 噪声扰动
    // 用于在不动 HE 代码的前提下，验证 PRO 是否能救回 K≥3 的精度。
    //
    // 噪声模型（每步注入相对幅度 sigma 的高斯扰动）：
    //   W   <- C·V * (1 + sigma*sqrt(d) * randn)         （matvec 内积放大 √d 倍）
    //   α   <- V^T·W * (1 + sigma*sqrt(d) * randn)        （内积放大 √d 倍）
    //   ‖W_new‖² <- 真值 * (1 + sigma * randn)            （单次平方求和）
    //   β   <- sqrt(‖W_new‖²) * (1 + 0.05*randn)         （Newton 2 轮 ~5% 误差）
    //
    // 重正交化：每步在 W_new = W − αV 之后，对最近 reorth_b 个基向量做 GS：
    //   reorth_b = 0    -> 不做 PRO（与当前 HE 一致）
    //   reorth_b = 2    -> Local PRO，对 V_{j-1}, V_{j-2} 做 GS
    //   reorth_b >= m   -> 完整 FRO，对所有 V_0..V_{j-1} 做 GS
    struct LanczosPROResult {
        Eigen::MatrixXd T;         // m × m 三对角
        Eigen::MatrixXd V_total;   // d × m 基向量
        int actual_m;              // 实际完成的步数（早停时 < m_iter）
    };

    LanczosPROResult plaintextLanczosWithPROAndNoise(
        const Eigen::VectorXd& v0,
        int m_iter,
        int reorth_b,
        double sigma,
        unsigned seed) const;

    // HE-Lanczos 的"明文逐操作镜像"，用于把 FHE 增量误差与迭代算法误差分离。
    //   - 算法路径与 server.cpp::lanczosIteration 完全一致：
    //       W = C·V − β_prev·V_prev   →   α = V·W   →   W_new = W − α·V
    //     可选 FRO（含 fro_skip_first 与 HE 端语义对齐）→ β = ‖W_new‖。
    //   - 用 std::sqrt 替代 Newton 1/√x：隔离 Newton 近似误差（误差源 C）。
    //   - 不注入任何噪声：隔离 CKKS 噪声（误差源 A）。
    //   返回的 T、V_total 经过 CW + reconstructEigenvectors 后即得
    //   "Krylov 子空间 m 步 + 浮点精度"能达到的最佳 Ritz 对，
    //   该结果 vs Eigen 真值 = B1（迭代不完备） + B2（浮点正交性丢失）。
    LanczosPROResult plaintextHeMirrorLanczos(
        const Eigen::VectorXd& v0,
        int m_iter,
        bool enable_fro,
        int fro_skip_first) const;

    // 与 plaintextHeMirrorLanczos 相同的明文逐操作镜像，但归一化阶段不用
    // std::sqrt，而是复现 HE 端 Newton 1/sqrt(x) 的初值与迭代次数。
    // 用于把误差拆成：sqrt mirror（Krylov 极限）→ Newton mirror（归一化近似）
    // → HE（CKKS/bootstrap 噪声 + Newton）。
    LanczosPROResult plaintextHeNewtonMirrorLanczos(
        const Eigen::VectorXd& v0,
        int m_iter,
        bool enable_fro,
        int fro_skip_first,
        double eigenvalue_guess,
        const std::vector<double>& per_iter_eigenvalue_guesses,
        int newton_iters) const;

    std::vector<double> plaintextMirrorResidualNormSqGuesses(
        const Eigen::VectorXd& v0,
        int m_iter) const;

    Eigen::MatrixXd plaintextStandardLanczosTridiagonal(
        const Eigen::VectorXd& v0,
        int m_iter) const;

    Eigen::MatrixXd trueTopEigenvectors(int K) const;

    double normalizeCovariance();

    Eigen::MatrixXd decryptLanczosVectors(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& V_all) const;

    Eigen::MatrixXd reconstructEigenvectors(
        const Eigen::MatrixXd& V_total,
        const CWFilterResult& cw,
        int K) const;

    // 访问器
    lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cryptoContext() const { return cc_; }
    lbcrypto::PublicKey<lbcrypto::DCRTPoly> publicKey() const { return keys_.publicKey; }
    lbcrypto::PrivateKey<lbcrypto::DCRTPoly> secretKey() const { return keys_.secretKey; }
    uint32_t numSlots() const { return num_slots_; }
    bool bootstrapEnabled() const { return bootstrap_enabled_; }
    uint32_t multiplicativeDepth() const { return multiplicative_depth_; }

    const Eigen::MatrixXd& covMatrix() const { return C_; }
    int d() const { return d_; }
    int p() const { return p_; }
    int m() const { return m_; }

    double eigenvalueMagnitudeGuess() const;
    double lanczosResidualNormSqGuess() const;

    // 推荐 BSGS 因子 b：选择 d 的因子使 |b - sqrt(d)| 最小
    //   d = 16  → 4
    //   d = 64  → 8
    //   d = 256 → 16
    //   d = 1024→ 32
    // 当 d 是 1 或质数（无非平凡因子）时返回 0（=不启用 BSGS）
    int autoBsgsB() const;

private:
    int d_, p_, m_;
    bool bootstrap_enabled_;
    double trace_C_{0.0};

    lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc_;
    lbcrypto::KeyPair<lbcrypto::DCRTPoly> keys_;
    uint32_t num_slots_{0};
    uint32_t multiplicative_depth_{0};

    Eigen::MatrixXd C_;
    Eigen::MatrixXd X_centered_;
    Eigen::RowVectorXd sample_mean_;

    void setupCryptoContext(bool enable_bootstrap, uint32_t levels_after_bootstrap);
};
