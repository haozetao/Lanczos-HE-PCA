#pragma once

#include <memory>

#include <seal/seal.h>

// ============================================================================
// CKKS 密文上的 Newton 迭代求 1/√x
//
// 迭代: y_{k+1} = y_k * (3 - x * y_k^2) / 2
//
// 深度: 每轮约 3 次密文乘法 (square/relinearize/rescale, x*y^2, y*diff) +
//       一次明文乘 0.5 (仍会 rescale 消耗层级，视 SEAL 版本而定)
//
// 注意: x 的密文在每轮需 mod_switch 到与中间结果一致的 parms_id
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

    InvSqrtResult compute(
        const seal::Ciphertext& x_ct,
        double initial_guess,
        int iterations) const;

private:
    std::shared_ptr<seal::SEALContext> context_;
    seal::Evaluator& evaluator_;
    seal::CKKSEncoder& encoder_;
    seal::Encryptor& encryptor_;
    const seal::RelinKeys& relin_keys_;
    double scale_;

    // 对齐两个密文到同一 parms（仅允许向下 mod_switch 较浅的一方）
    static void align_two(
        seal::Evaluator& ev,
        const seal::SEALContext& ctx,
        seal::Ciphertext& a,
        seal::Ciphertext& b);

    void adjust_scale_to_ref_inplace_(seal::Ciphertext& moving, const seal::Ciphertext& ref) const;
};
