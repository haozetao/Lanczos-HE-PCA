#pragma once

#include <memory>
#include <vector>

#include <seal/seal.h>

#include "newton_inv_sqrt.h"

// ============================================================================
// Server —— 公钥 + 计算密钥；方向2：完整密态 Lanczos（p=1）+ Newton 归一化
// ============================================================================
class Server {
public:
    Server(std::shared_ptr<seal::SEALContext> ctx,
           const seal::PublicKey& pk,
           const seal::RelinKeys& rlk,
           const seal::GaloisKeys& glk,
           double ckks_scale);

    std::vector<seal::Ciphertext> matmul(
        const std::vector<seal::Ciphertext>& enc_C,
        const std::vector<seal::Ciphertext>& enc_V,
        int d) const;

    struct LanczosResult {
        std::vector<seal::Ciphertext> alphas; // m 个 [[α_j]]，标量在 slot 0
        std::vector<seal::Ciphertext> betas;  // m-1 个 [[β_j]]
        std::vector<seal::Ciphertext> V_all;  // m 个基向量密文（用于特征向量重构）
    };

    // p 必须为 1（标准 Lanczos）。m_iter 受 CKKS 深度约束，建议 1。
    // asor_k_factors 非空时使用 aSOR Newton，为空时使用标准 Newton。
    LanczosResult lanczosIteration(
        const std::vector<seal::Ciphertext>& enc_C,
        const std::vector<seal::Ciphertext>& enc_V1,
        int d,
        int p,
        int m_iter,
        double eigenvalue_guess,
        int newton_iters,
        const std::vector<double>& asor_k_factors = {});

private:
    std::shared_ptr<seal::SEALContext> context_;
    seal::Evaluator evaluator_;
    seal::CKKSEncoder encoder_;
    seal::Encryptor encryptor_;
    seal::RelinKeys relin_keys_;
    seal::GaloisKeys galois_keys_;
    double scale_;

    std::unique_ptr<NewtonInvSqrt> newton_;

    void align_two_inplace(seal::Ciphertext& a, seal::Ciphertext& b) const;

    // 用 multiply_plain(全 1) 将 moving 的 scale 对齐到 ref（CKKS 同 parms 下 ratio 不变时 mod_switch 无效）
    void adjust_ckks_scale_to_ref_inplace_(
        seal::Ciphertext& moving, const seal::Ciphertext& ref) const;

    seal::Ciphertext innerProduct(
        const seal::Ciphertext& a,
        const seal::Ciphertext& b,
        int dim) const;

    seal::Ciphertext broadcastScalar(const seal::Ciphertext& scalar_ct, int dim) const;

    seal::Ciphertext scalarVecMultiply(
        const seal::Ciphertext& broadcasted_or_scalar,
        const seal::Ciphertext& vec_ct,
        int dim) const;

    seal::Ciphertext packScalarsToVector(
        const std::vector<seal::Ciphertext>& scalar_cts, int d) const;
};
