#pragma once

#include <memory>
#include <vector>

#include <openfhe.h>

#include "newton_inv_sqrt.h"

// ============================================================================
// Server —— OpenFHE CKKS 版本
//   - 方向2：完整密态 Lanczos（p=1）+ Newton 归一化
//   - 支持可选 Bootstrap：当剩余深度不够继续 Lanczos 时自动刷新密文
// ============================================================================
class Server {
public:
    Server(lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc,
           lbcrypto::PublicKey<lbcrypto::DCRTPoly> pk,
           uint32_t total_depth,
           bool enable_bootstrap,
           uint32_t num_slots);

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> matmul(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_C,
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_V,
        int d) const;

    struct LanczosResult {
        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> alphas;
        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> betas;
        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> V_all;
    };

    // Bootstrap 相关统计（由 lanczosIteration 填充）
    struct LanczosStats {
        int bootstrap_count = 0;
        double bootstrap_ms_total = 0.0;
    };

    LanczosResult lanczosIteration(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_C,
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_V1,
        int d,
        int p,
        int m_iter,
        double eigenvalue_guess,
        const std::vector<double>& per_iter_eigenvalue_guesses,
        int newton_iters,
        const std::vector<double>& asor_k_factors = {},
        LanczosStats* stats_out = nullptr,
        bool enable_fro = false,
        int fro_skip_first = 2);

private:
    lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc_;
    lbcrypto::PublicKey<lbcrypto::DCRTPoly> pk_;
    uint32_t total_depth_;
    bool bootstrap_enabled_;
    uint32_t num_slots_;
    mutable std::vector<lbcrypto::Plaintext> slot_masks_;

    std::unique_ptr<NewtonInvSqrt> newton_;

    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> innerProduct(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& a,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& b,
        int dim) const;

    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> scalarVecMultiply(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& scalar_ct,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& vec_ct) const;

    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> packScalarsToVector(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& scalar_cts,
        int d) const;

    void ensureSlotMasks(int d) const;

    // 剩余乘法深度 ≤ min_required 时触发 Bootstrap，返回是否实际执行
    bool bootstrapIfNeeded(
        lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct,
        uint32_t min_required,
        LanczosStats* stats) const;

    uint32_t remainingDepth(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct) const;
};
