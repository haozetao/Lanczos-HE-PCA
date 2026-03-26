#pragma once

#include <memory>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <seal/seal.h>

// ============================================================================
// Client 类 —— 持有私钥，负责加解密、明文侧 Lanczos 校正与特征分解
// ============================================================================
class Client {
public:
    // 构造：初始化 CKKS 上下文与全部密钥
    //   d = 数据维度, p = Block 大小, m = Lanczos 迭代次数
    Client(int d = 10, int p = 2, int m = 3);

    // ── Phase 1 ─────────────────────────────────────────────────────────────

    // 生成随机对称正定协方差矩阵 C (d×d)
    void generateCovarianceMatrix();

    // 行打包加密 C → d 个密文, enc_C[i] 的前 d 个 slot 存放 C 的第 i 行
    std::vector<seal::Ciphertext> encryptCovMatrix() const;

    // 列打包加密任意 d×p 矩阵 V → p 个密文, enc_V[j] 的前 d 个 slot 存放 V 的第 j 列
    std::vector<seal::Ciphertext> encryptBlockVec(
        const Eigen::MatrixXd& V) const;

    // 生成初始随机块向量 V1 (d×p), 经 QR 正交归一化后返回
    Eigen::MatrixXd generateInitialBlock() const;

    // ── Phase 2 ─────────────────────────────────────────────────────────────

    // 解密 Server 返回的 d*p 个标量密文, 重建 d×p 明文矩阵
    // 密文按行优先排列: enc_W[i*p+j] 对应矩阵 (i, j)
    Eigen::MatrixXd decryptToMatrix(
        const std::vector<seal::Ciphertext>& enc_W,
        int rows, int cols) const;

    // 强制完全正交化: 将 W 与所有历史基块 V_history 正交
    static Eigen::MatrixXd fullReorthogonalize(
        const Eigen::MatrixXd& W,
        const std::vector<Eigen::MatrixXd>& V_history);

    // 对 d×p 矩阵做 Householder QR 分解, 返回 {Q, R}
    //   Q (d×p): 正交归一化后的新基底 V_{j+1}
    //   R (p×p): 上三角矩阵 B_j
    static std::pair<Eigen::MatrixXd, Eigen::MatrixXd> qrDecompose(
        const Eigen::MatrixXd& W);

    // ── Phase 3 ─────────────────────────────────────────────────────────────

    // 用收集到的 A_list / B_list 拼装 (m*p)×(m*p) 块三对角对称矩阵 T_m
    Eigen::MatrixXd buildBlockTridiag() const;

    // 对 C 做直接特征分解, 返回前 K 个最大特征值对应的特征向量 (d×K)
    Eigen::MatrixXd trueTopEigenvectors(int K) const;

    // ── 密钥 / 上下文 访问 ──────────────────────────────────────────────────

    std::shared_ptr<seal::SEALContext> context() const { return context_; }
    const seal::PublicKey&  publicKey()  const { return public_key_; }
    const seal::RelinKeys&  relinKeys()  const { return relin_keys_; }
    const seal::GaloisKeys& galoisKeys() const { return galois_keys_; }

    // ── 数据访问 ────────────────────────────────────────────────────────────

    const Eigen::MatrixXd& covMatrix() const { return C_; }
    int d() const { return d_; }
    int p() const { return p_; }
    int m() const { return m_; }

    std::vector<Eigen::MatrixXd>& vHistory() { return V_history_; }
    std::vector<Eigen::MatrixXd>& aList()    { return A_list_; }
    std::vector<Eigen::MatrixXd>& bList()    { return B_list_; }

private:
    int d_, p_, m_;
    double scale_;

    std::shared_ptr<seal::SEALContext> context_;
    seal::SecretKey  secret_key_;
    seal::PublicKey   public_key_;
    seal::RelinKeys   relin_keys_;
    seal::GaloisKeys  galois_keys_;

    std::unique_ptr<seal::CKKSEncoder> encoder_;
    std::unique_ptr<seal::Encryptor>   encryptor_;
    std::unique_ptr<seal::Decryptor>   decryptor_;

    Eigen::MatrixXd C_;                          // 协方差矩阵
    std::vector<Eigen::MatrixXd> V_history_;     // V_1 … V_m
    std::vector<Eigen::MatrixXd> A_list_;        // A_1 … A_m
    std::vector<Eigen::MatrixXd> B_list_;        // B_1 … B_{m-1}
};
