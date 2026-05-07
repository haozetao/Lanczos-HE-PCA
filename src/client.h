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
    Client(int d = 10,
           int p = 1,
           int m = 3,
           bool enable_bootstrap = false,
           uint32_t levels_after_bootstrap = 12);

    void generateCovarianceMatrix();

    // 生成低秩 + 噪声的合成数据集，用于 R²(X) 重建评估
    //   N         : 样本数
    //   true_rank : 真实低秩结构的秩（越小越利于 PCA 压缩）
    //   noise_sigma : 加性高斯噪声标准差（0 → 纯低秩）
    // 调用后内部保存：
    //   X_centered_ ∈ R^{N×d} （已减去均值）
    //   C_         = X_centered_^T · X_centered_ / (N-1)
    void generateLowRankDataset(int N, int true_rank, double noise_sigma);

    // 已中心化的数据矩阵（仅在 generateLowRankDataset 后有效）
    const Eigen::MatrixXd& centeredData() const { return X_centered_; }
    bool hasData() const { return X_centered_.rows() > 0; }

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptCovMatrix() const;

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptBlockVec(
        const Eigen::MatrixXd& V) const;

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> encryptColumnVector(
        const Eigen::VectorXd& v) const;

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

    void setupCryptoContext(bool enable_bootstrap, uint32_t levels_after_bootstrap);
};
