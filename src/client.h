#pragma once

#include <memory>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <seal/seal.h>

// Cullum-Willoughby 过滤结果
struct CWFilterResult {
    Eigen::VectorXd good_eigenvalues;
    Eigen::MatrixXd good_eigenvectors;
};

// ============================================================================
// Client —— 方向2：深 CKKS 参数、初始加密、解密后 CW 过滤与三对角特征分解
// ============================================================================
class Client {
public:
    Client(int d = 10, int p = 1, int m = 3);

    void generateCovarianceMatrix();

    std::vector<seal::Ciphertext> encryptCovMatrix() const;

    std::vector<seal::Ciphertext> encryptBlockVec(
        const Eigen::MatrixXd& V) const;

    // p=1 时便捷接口：单列向量 v ∈ R^d
    std::vector<seal::Ciphertext> encryptColumnVector(
        const Eigen::VectorXd& v) const;

    Eigen::MatrixXd decryptToMatrix(
        const std::vector<seal::Ciphertext>& enc_W,
        int rows, int cols) const;

    // 解密 [[α]]、[[β]] 并组装对称三对角 T_m（标量 Lanczos，非块）
    Eigen::MatrixXd buildTridiagonalFromEncrypted(
        const std::vector<seal::Ciphertext>& enc_alphas,
        const std::vector<seal::Ciphertext>& enc_betas,
        int m) const;

    // m<2 时跳过 CW，直接取前 K 个最大特征值
    CWFilterResult cullumWilloughbyFilter(
        const Eigen::MatrixXd& T_m,
        int K) const;

    // 与 Server::lanczosIteration 数学形式一致的明文三对角（用于在无边自举前标定 m）
    Eigen::MatrixXd plaintextMirrorHeLanczosTridiagonal(
        const Eigen::VectorXd& v0,
        int m_iter) const;

    // 标准对称 Lanczos（含 β_{j-1} v_{j-1}），用于对照「理论上 m 需多大」
    Eigen::MatrixXd plaintextStandardLanczosTridiagonal(
        const Eigen::VectorXd& v0,
        int m_iter) const;

    Eigen::MatrixXd trueTopEigenvectors(int K) const;

    // 明文空间归一化：C_hat = C / trace(C)，返回 trace 值用于反归一化
    double normalizeCovariance();

    // 解密 Lanczos 基向量密文 → 明文矩阵 V_total (d x m)
    Eigen::MatrixXd decryptLanczosVectors(
        const std::vector<seal::Ciphertext>& V_all) const;

    // 从 CW 结果和 Lanczos 基向量重构特征向量
    // 包含明文 Gram-Schmidt 正交化以修复三项递推导致的正交性丧失
    Eigen::MatrixXd reconstructEigenvectors(
        const Eigen::MatrixXd& V_total,
        const CWFilterResult& cw,
        int K) const;

    std::shared_ptr<seal::SEALContext> context() const { return context_; }
    const seal::PublicKey& publicKey() const { return public_key_; }
    const seal::RelinKeys& relinKeys() const { return relin_keys_; }
    const seal::GaloisKeys& galoisKeys() const { return galois_keys_; }

    double ckksScale() const { return scale_; }

    const Eigen::MatrixXd& covMatrix() const { return C_; }
    int d() const { return d_; }
    int p() const { return p_; }
    int m() const { return m_; }

    double eigenvalueMagnitudeGuess() const;

    // 估计 Lanczos 第一步残差向量的 ||W_new||²
    // 用作 Newton 1/√x 的初始猜测，比 eigenvalueMagnitudeGuess 精确得多
    double lanczosResidualNormSqGuess() const;

private:
    int d_, p_, m_;
    double scale_;
    double trace_C_{0.0};

    std::shared_ptr<seal::SEALContext> context_;
    seal::SecretKey secret_key_;
    seal::PublicKey public_key_;
    seal::RelinKeys relin_keys_;
    seal::GaloisKeys galois_keys_;

    std::unique_ptr<seal::CKKSEncoder> encoder_;
    std::unique_ptr<seal::Encryptor> encryptor_;
    std::unique_ptr<seal::Decryptor> decryptor_;

    Eigen::MatrixXd C_;
};
