#include "client.h"

#include <cmath>
#include <iostream>
#include <random>

// ============================================================================
// 构造函数 —— 初始化 CKKS 上下文 & 生成全部密钥
// ============================================================================
Client::Client(int d, int p, int m)
    : d_(d), p_(p), m_(m), scale_(std::pow(2.0, 40))
{
    // ── CKKS 参数 ───────────────────────────────────────────────────────
    // poly_modulus_degree = 8192  →  slot_count = 4096
    // coeff_modulus bit 长度: {60, 40, 40, 60}
    //   - 首尾 60-bit 用于 special prime
    //   - 中间两个 40-bit 提供 2 层乘法深度
    seal::EncryptionParameters params(seal::scheme_type::ckks);
    params.set_poly_modulus_degree(8192);
    params.set_coeff_modulus(
        seal::CoeffModulus::Create(8192, {60, 40, 40, 60}));

    context_ = std::make_shared<seal::SEALContext>(params);

    // ── 密钥生成 ────────────────────────────────────────────────────────
    seal::KeyGenerator keygen(*context_);
    secret_key_ = keygen.secret_key();
    keygen.create_public_key(public_key_);
    keygen.create_relin_keys(relin_keys_);

    // rotate-and-sum 所需的旋转步长: 1, 2, 4, 8 (足以累加 d<=16 个 slot)
    std::vector<int> rot_steps = {1, 2, 4, 8};
    keygen.create_galois_keys(rot_steps, galois_keys_);

    // ── 编码器 / 加密器 / 解密器 ────────────────────────────────────────
    encoder_   = std::make_unique<seal::CKKSEncoder>(*context_);
    encryptor_ = std::make_unique<seal::Encryptor>(*context_, public_key_);
    decryptor_ = std::make_unique<seal::Decryptor>(*context_, secret_key_);
}

// ============================================================================
// Phase 1: 生成随机对称正定协方差矩阵
// ============================================================================
void Client::generateCovarianceMatrix()
{
    // 构造方式: C = A^T * A + I, 确保对称正定
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.0, 1.0);

    Eigen::MatrixXd A(d_, d_);
    for (int i = 0; i < d_; ++i)
        for (int j = 0; j < d_; ++j)
            A(i, j) = dist(rng);

    C_ = A.transpose() * A + Eigen::MatrixXd::Identity(d_, d_);
}

// ============================================================================
// Phase 1: 行打包加密协方差矩阵 C
//
// 打包策略: enc_C[i] 的前 d 个 slot 存放 C 的第 i 行,
//           剩余 slot 填 0 (slot_count = 4096 >> d = 10).
//           这样与列打包的 V 做逐元素乘再 rotate-and-sum 即可得到点积.
// ============================================================================
std::vector<seal::Ciphertext> Client::encryptCovMatrix() const
{
    size_t slot_count = encoder_->slot_count();
    std::vector<seal::Ciphertext> enc_C(d_);

    for (int i = 0; i < d_; ++i) {
        std::vector<double> row(slot_count, 0.0);
        for (int k = 0; k < d_; ++k)
            row[k] = C_(i, k);

        seal::Plaintext pt;
        encoder_->encode(row, scale_, pt);
        encryptor_->encrypt(pt, enc_C[i]);
    }
    return enc_C;
}

// ============================================================================
// Phase 1 / 2: 列打包加密 d×p 矩阵 V
//
// 打包策略: enc_V[j] 的前 d 个 slot 存放 V 的第 j 列,
//           即 [V(0,j), V(1,j), …, V(d-1,j), 0, …]
// ============================================================================
std::vector<seal::Ciphertext> Client::encryptBlockVec(
    const Eigen::MatrixXd& V) const
{
    size_t slot_count = encoder_->slot_count();
    int cols = static_cast<int>(V.cols());
    std::vector<seal::Ciphertext> enc_V(cols);

    for (int j = 0; j < cols; ++j) {
        std::vector<double> col(slot_count, 0.0);
        for (int i = 0; i < d_; ++i)
            col[i] = V(i, j);

        seal::Plaintext pt;
        encoder_->encode(col, scale_, pt);
        encryptor_->encrypt(pt, enc_V[j]);
    }
    return enc_V;
}

// ============================================================================
// Phase 1: 生成初始随机块向量并 QR 正交归一化
// ============================================================================
Eigen::MatrixXd Client::generateInitialBlock() const
{
    std::mt19937 rng(123);
    std::normal_distribution<double> dist(0.0, 1.0);

    Eigen::MatrixXd V1(d_, p_);
    for (int i = 0; i < d_; ++i)
        for (int j = 0; j < p_; ++j)
            V1(i, j) = dist(rng);

    // Householder QR → 取正交归一化的 Q 矩阵
    Eigen::HouseholderQR<Eigen::MatrixXd> qr(V1);
    Eigen::MatrixXd Q = qr.householderQ()
                            * Eigen::MatrixXd::Identity(d_, p_);
    return Q;
}

// ============================================================================
// Phase 2: 解密 Server 返回的 d*p 个标量密文 → d×p 明文矩阵
//
// 每个密文的 slot 0 存放一个标量 (对应矩阵的一个元素).
// 排列: enc_W[i*cols + j] ←→ result(i, j)
// ============================================================================
Eigen::MatrixXd Client::decryptToMatrix(
    const std::vector<seal::Ciphertext>& enc_W,
    int rows, int cols) const
{
    Eigen::MatrixXd result(rows, cols);

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            seal::Plaintext pt;
            decryptor_->decrypt(enc_W[i * cols + j], pt);

            std::vector<double> decoded;
            encoder_->decode(pt, decoded);

            result(i, j) = decoded[0];
        }
    }
    return result;
}

// ============================================================================
// Phase 2: 强制完全正交化 (Gram-Schmidt 纠偏)
//
//   W_hat = W - sum_i V_i * (V_i^T * W)
//
// 消除 HE 近似运算带来的正交性漂移, 确保 Lanczos 基绝对正交.
// ============================================================================
Eigen::MatrixXd Client::fullReorthogonalize(
    const Eigen::MatrixXd& W,
    const std::vector<Eigen::MatrixXd>& V_history)
{
    Eigen::MatrixXd W_hat = W;
    // 执行两遍以提高数值稳定性 (经典 double Gram-Schmidt)
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& Vi : V_history) {
            // 投影系数 (p×p)
            Eigen::MatrixXd coeff = Vi.transpose() * W_hat;
            W_hat -= Vi * coeff;
        }
    }
    return W_hat;
}

// ============================================================================
// Phase 2: QR 分解 → 返回 {Q (d×p), R (p×p)}
// ============================================================================
std::pair<Eigen::MatrixXd, Eigen::MatrixXd> Client::qrDecompose(
    const Eigen::MatrixXd& W)
{
    Eigen::HouseholderQR<Eigen::MatrixXd> qr(W);

    int rows = static_cast<int>(W.rows());
    int cols = static_cast<int>(W.cols());

    Eigen::MatrixXd Q = qr.householderQ()
                            * Eigen::MatrixXd::Identity(rows, cols);

    // R 是 p×p 上三角矩阵
    Eigen::MatrixXd R = qr.matrixQR()
                            .topLeftCorner(cols, cols)
                            .triangularView<Eigen::Upper>();

    // 保证 R 的对角元素为正 (消除 QR 的符号歧义, 保持一致性)
    for (int j = 0; j < cols; ++j) {
        if (R(j, j) < 0.0) {
            R.row(j)   *= -1.0;
            Q.col(j)   *= -1.0;
        }
    }

    return {Q, R};
}

// ============================================================================
// Phase 3: 拼装 (m*p)×(m*p) 块三对角对称矩阵 T_m
//
//   T_m = | A_1    B_1^T              |
//         | B_1    A_2    B_2^T       |
//         |        B_2    A_3   ...   |
//         |              ...          |
// ============================================================================
Eigen::MatrixXd Client::buildBlockTridiag() const
{
    int mp = m_ * p_;
    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(mp, mp);

    for (int j = 0; j < m_; ++j) {
        int off = j * p_;
        // 对角块 A_{j+1}
        T.block(off, off, p_, p_) = A_list_[j];

        // 次对角块: 只有 m-1 个 B (B_1 .. B_{m-1})
        if (j < m_ - 1 && j < static_cast<int>(B_list_.size())) {
            T.block(off + p_, off, p_, p_) = B_list_[j];       // 下三角
            T.block(off, off + p_, p_, p_) = B_list_[j].transpose(); // 上三角
        }
    }

    // 强制对称 (消除浮点误差)
    T = (T + T.transpose()) / 2.0;
    return T;
}

// ============================================================================
// Phase 3 验证: 对原始 C 做直接特征分解, 返回前 K 个最大特征值对应的特征向量
// ============================================================================
Eigen::MatrixXd Client::trueTopEigenvectors(int K) const
{
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(C_);
    // Eigen 返回的特征值按升序排列, 取最后 K 列 (最大特征值对应)
    Eigen::MatrixXd vecs = solver.eigenvectors().rightCols(K);
    // 翻转列序使第一列对应最大特征值
    return vecs.rowwise().reverse();
}
