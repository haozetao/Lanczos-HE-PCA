#include "newton_inv_sqrt.h"

#include <stdexcept>

using namespace lbcrypto;

NewtonInvSqrt::NewtonInvSqrt(CryptoContext<DCRTPoly> cc, PublicKey<DCRTPoly> pk)
    : cc_(std::move(cc)), pk_(std::move(pk))
{
}

Ciphertext<DCRTPoly> NewtonInvSqrt::encryptConstant(double v) const
{
    uint32_t slots = cc_->GetRingDimension() / 2;
    std::vector<double> vec(slots, v);
    Plaintext pt = cc_->MakeCKKSPackedPlaintext(vec);
    return cc_->Encrypt(pk_, pt);
}

Ciphertext<DCRTPoly> NewtonInvSqrt::newtonStep(
    const Ciphertext<DCRTPoly>& x_ct,
    const Ciphertext<DCRTPoly>& y,
    double half_factor) const
{
    // y^2
    auto y_sq = cc_->EvalMult(y, y);

    // x * y^2
    auto x_y_sq = cc_->EvalMult(x_ct, y_sq);

    // t = 3 - x*y^2
    auto t = cc_->EvalSub(3.0, x_y_sq);

    // y * t * half_factor = y * (3 - x*y^2) * half_factor
    // 将常数 half_factor 吸收进一次乘法，保持 3 次乘法深度
    auto yt = cc_->EvalMult(y, t);
    auto out = cc_->EvalMult(yt, half_factor);
    return out;
}

InvSqrtResult NewtonInvSqrt::compute(
    const Ciphertext<DCRTPoly>& x_ct,
    double initial_guess,
    int iterations) const
{
    auto y = encryptConstant(initial_guess);

    for (int it = 0; it < iterations; ++it) {
        y = newtonStep(x_ct, y, 0.5);
    }

    // sqrt(x) = x * (1/sqrt(x))
    auto sqrt_val = cc_->EvalMult(x_ct, y);

    return InvSqrtResult{y, sqrt_val};
}

InvSqrtResult NewtonInvSqrt::computeWithASOR(
    const Ciphertext<DCRTPoly>& x_ct,
    double initial_guess,
    const std::vector<double>& k_factors) const
{
    const int n = static_cast<int>(k_factors.size());
    if (n < 1) {
        throw std::invalid_argument("computeWithASOR: k_factors must not be empty");
    }

    // 将 k_1 吸收进初始猜测
    auto y = encryptConstant(k_factors[0] * initial_guess);

    for (int it = 0; it < n; ++it) {
        double hf = (it < n - 1) ? k_factors[it + 1] / 2.0 : 0.5;
        y = newtonStep(x_ct, y, hf);
    }

    auto sqrt_val = cc_->EvalMult(x_ct, y);
    return InvSqrtResult{y, sqrt_val};
}
