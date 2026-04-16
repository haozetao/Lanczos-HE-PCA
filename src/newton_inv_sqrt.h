#pragma once

#include <memory>
#include <vector>

#include <seal/seal.h>

// ============================================================================
// CKKS 密文上的 Newton 迭代求 1/√x
//
// 标准迭代: y_{k+1} = y_k * (3 - x * y_k^2) / 2
// aSOR 迭代: y'_i = k_i * y_i; y_{i+1} = y'_i * (3 - x * y'_i^2) / 2
//
// 深度: 每轮 4 次 rescale (square, x*y^2, y*t, *0.5)
//
// aSOR 通过将 k_i 吸收到 0.5 常数中实现零额外深度消耗：
//   - 初始猜测加密为 k_1 * initial_guess
//   - 中间迭代的 0.5 替换为 k_{i+1}/2
//   - 最后一次迭代正常乘 0.5
// ============================================================================

struct InvSqrtResult {
    seal::Ciphertext inv_sqrt; // 近似 1/√x，有效信息在 slot 0
    seal::Ciphertext sqrt_val; // x * (1/√x) ≈ √x
};

class NewtonInvSqrt {
public:
    NewtonInvSqrt(
        std::shared_ptr<seal::SEALContext> context,
        seal::Evaluator& evaluator,
        seal::CKKSEncoder& encoder,
        seal::Encryptor& encryptor,
        const seal::RelinKeys& relin_keys,
        double scale);

    // 标准 Newton 迭代（无 aSOR 加速）
    InvSqrtResult compute(
        const seal::Ciphertext& x_ct,
        double initial_guess,
        int iterations) const;

    // aSOR 加速 Newton 迭代
    // k_factors: 预计算的松弛因子序列（来自 precomputeInvSqrtASOR）
    // k_factors.size() 决定迭代次数
    InvSqrtResult computeWithASOR(
        const seal::Ciphertext& x_ct,
        double initial_guess,
        const std::vector<double>& k_factors) const;

private:
    std::shared_ptr<seal::SEALContext> context_;
    seal::Evaluator& evaluator_;
    seal::CKKSEncoder& encoder_;
    seal::Encryptor& encryptor_;
    const seal::RelinKeys& relin_keys_;
    double scale_;

    static void align_two(
        seal::Evaluator& ev,
        const seal::SEALContext& ctx,
        seal::Ciphertext& a,
        seal::Ciphertext& b);

    void adjust_scale_to_ref_inplace_(seal::Ciphertext& moving, const seal::Ciphertext& ref) const;

    // 内部：执行一轮 Newton 步 y' * (3 - x*y'^2) * half_factor
    // half_factor 通常为 0.5，aSOR 中为 k_{i+1}/2 或 0.5（末轮）
    seal::Ciphertext newtonStep(
        const seal::Ciphertext& x_ct,
        seal::Ciphertext& y,
        double half_factor) const;
};
