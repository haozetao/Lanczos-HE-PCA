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

    // ── Hybrid (BSGS) Diagonal matvec ─────────────────────────────────────────
    // 输入：enc_C_diag (d 个 ct，由 Client::encryptCovMatrixDiagonal(b) 编码)
    //      v_ct       (1 个 replicated ct)
    // 输出：result_ct = C·v 的 replicated 编码（同 v_ct 布局）
    // 当 bsgs_b == 0：plain diagonal，d 次 Rotate + d 次 Mult。
    // 当 bsgs_b > 0 ：BSGS，a+b ≈ 2√d 次 Rotate + d 次 Mult。
    //                 要求 enc_C_diag 编码时使用了相同的 bsgs_b。
    // 仅消耗 1 层乘法深度（对 v_ct）。
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> matvecDiagonal(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_C_diag,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& v_ct,
        int d,
        int bsgs_b = 0) const;

    // 对 replicated 编码的两个 ct 求 V·W：
    //   product = V ⊙ W
    //   sum over d slots (而非全 num_slots)
    //   结果在每个 slot 都是精确 V·W （replicated 编码）
    // 只需 log_2(d) 次 Rotate，比旧 innerProduct 少 log(num_slots/d) 次。
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> innerProductReplicated(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& a,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& b,
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

    // Lanczos 迭代 —— Diagonal Packing 版本
    //   enc_C_diag : d 个 ct，Client::encryptCovMatrixDiagonal(bsgs_b) 编码
    //   v1_ct      : 1 个 replicated ct
    //   bsgs_b     : 与编码时一致；0 表示无 BSGS
    LanczosResult lanczosIteration(
        const std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>>& enc_C_diag,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& v1_ct,
        int d,
        int m_iter,
        double eigenvalue_guess,
        const std::vector<double>& per_iter_eigenvalue_guesses,
        int newton_iters,
        int bsgs_b = 0,
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

    std::unique_ptr<NewtonInvSqrt> newton_;

    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> scalarVecMultiply(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& scalar_ct,
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& vec_ct) const;

    // 剩余乘法深度 ≤ min_required 时触发 Bootstrap，返回是否实际执行
    bool bootstrapIfNeeded(
        lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct,
        uint32_t min_required,
        LanczosStats* stats) const;

    uint32_t remainingDepth(
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct) const;
};
