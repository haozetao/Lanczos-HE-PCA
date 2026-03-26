#pragma once

#include <memory>
#include <vector>

#include <seal/seal.h>

// ============================================================================
// Server 类 —— 持有公钥与计算密钥，仅执行密态矩阵乘法
//
// 方法签名严格约束: 参数仅含 seal::Ciphertext / seal::Plaintext / int,
// 不接受任何明文 Eigen 类型.
// ============================================================================
class Server {
public:
    Server(std::shared_ptr<seal::SEALContext> ctx,
           const seal::PublicKey&  pk,
           const seal::RelinKeys&  rlk,
           const seal::GaloisKeys& glk);

    // ── 核心密态操作 ────────────────────────────────────────────────────────
    //
    // 密态矩阵乘: enc_C (d 个行打包密文) × enc_V (p 个列打包密文)
    //
    // 返回 d*p 个标量密文, 按行优先排列:
    //   result[i*p + j] 的 slot 0 = dot(C 第 i 行, V 第 j 列)
    //
    // 算法:
    //   对每个 (i, j) 对:
    //     1. element-wise multiply: enc_C[i] ⊙ enc_V[j]
    //     2. relinearize (降低密文大小)
    //     3. rescale (将 scale 从 2^80 降回 2^40)
    //     4. rotate-and-sum (步长 1,2,4,8) 将 d 个分量累加到 slot 0
    //
    std::vector<seal::Ciphertext> matmul(
        const std::vector<seal::Ciphertext>& enc_C,
        const std::vector<seal::Ciphertext>& enc_V,
        int d) const;

private:
    std::shared_ptr<seal::SEALContext> context_;
    seal::Evaluator   evaluator_;
    seal::CKKSEncoder encoder_;
    seal::RelinKeys   relin_keys_;
    seal::GaloisKeys  galois_keys_;

    // 辅助: 两个向量密文的内积 (element-wise multiply + rotate-and-sum)
    seal::Ciphertext innerProduct(
        const seal::Ciphertext& a,
        const seal::Ciphertext& b) const;
};
