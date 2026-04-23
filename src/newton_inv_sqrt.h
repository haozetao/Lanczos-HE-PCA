#pragma once

#include <vector>

#include <openfhe.h>

// ============================================================================
// CKKS 密文上的 Newton 迭代求 1/√x  (OpenFHE 版)
//
// 标准迭代: y_{k+1} = y_k * (3 - x * y_k^2) / 2
// aSOR 迭代: y'_i = k_i * y_i; y_{i+1} = y'_i * (3 - x * y'_i^2) / 2
//
// 每轮深度（FLEXIBLEAUTO 自动 rescale）: 约 3 乘法层
//   1) y^2           -> 1 层
//   2) x * y^2       -> 1 层
//   3) y * (3-x*y^2) -> 1 层 (包含 *0.5 作为常数乘)
// ============================================================================

struct InvSqrtResult {
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> inv_sqrt;
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> sqrt_val;
};

class NewtonInvSqrt {
public:
    NewtonInvSqrt(lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc,
                  lbcrypto::PublicKey<lbcrypto::DCRTPoly> pk);

    InvSqrtResult compute(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& x_ct,
        double initial_guess,
        int iterations) const;

    InvSqrtResult computeWithASOR(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& x_ct,
        double initial_guess,
        const std::vector<double>& k_factors) const;

private:
    lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc_;
    lbcrypto::PublicKey<lbcrypto::DCRTPoly> pk_;

    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> newtonStep(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& x_ct,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& y,
        double half_factor) const;

    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> encryptConstant(double v) const;
};
