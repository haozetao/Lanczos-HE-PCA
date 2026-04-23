#include "server.h"

#include <chrono>
#include <cmath>
#include <stdexcept>

using namespace lbcrypto;

Server::Server(CryptoContext<DCRTPoly> cc,
               PublicKey<DCRTPoly> pk,
               uint32_t total_depth,
               bool enable_bootstrap,
               uint32_t num_slots)
    : cc_(std::move(cc))
    , pk_(std::move(pk))
    , total_depth_(total_depth)
    , bootstrap_enabled_(enable_bootstrap)
    , num_slots_(num_slots)
{
    newton_ = std::make_unique<NewtonInvSqrt>(cc_, pk_);
}

uint32_t Server::remainingDepth(const Ciphertext<DCRTPoly>& ct) const
{
    // OpenFHE: level 字段表示已消耗的乘法层数
    uint32_t lvl = ct->GetLevel();
    if (lvl >= total_depth_) return 0;
    return total_depth_ - lvl;
}

bool Server::bootstrapIfNeeded(
    Ciphertext<DCRTPoly>& ct, uint32_t min_required, LanczosStats* stats) const
{
    if (!bootstrap_enabled_) return false;
    if (remainingDepth(ct) >= min_required) return false;

    auto t0 = std::chrono::high_resolution_clock::now();
    ct = cc_->EvalBootstrap(ct);
    auto t1 = std::chrono::high_resolution_clock::now();
    if (stats) {
        stats->bootstrap_count += 1;
        stats->bootstrap_ms_total +=
            std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    return true;
}

Ciphertext<DCRTPoly> Server::innerProduct(
    const Ciphertext<DCRTPoly>& a,
    const Ciphertext<DCRTPoly>& b,
    int /*dim*/) const
{
    // 逐元素乘积
    auto product = cc_->EvalMult(a, b);

    // 全 slot rotate-and-sum：旋转步长覆盖到 num_slots_/2
    // 必须覆盖全 slot 因为 packScalarsToVector 依赖每个 slot i 都含完整内积，
    // 不能只保证 slot 0 正确（下游 mask[i]=1 会从 slot i 取值）
    for (uint32_t step = 1; step < num_slots_; step *= 2) {
        auto rotated = cc_->EvalRotate(product, static_cast<int32_t>(step));
        product = cc_->EvalAdd(product, rotated);
    }
    return product;
}

Ciphertext<DCRTPoly> Server::scalarVecMultiply(
    const Ciphertext<DCRTPoly>& scalar_ct,
    const Ciphertext<DCRTPoly>& vec_ct) const
{
    // scalar_ct 来自 innerProduct，已全 slot 填充 → 直接逐元素乘
    return cc_->EvalMult(scalar_ct, vec_ct);
}

Ciphertext<DCRTPoly> Server::packScalarsToVector(
    const std::vector<Ciphertext<DCRTPoly>>& scalar_cts, int d) const
{
    // 每个 scalar_cts[i] 所有 slot 都含标量值
    // 用掩码 [0,...,1,...,0]（位置 i 为 1）提取到正确 slot，再累加
    Ciphertext<DCRTPoly> result;

    for (int i = 0; i < d; ++i) {
        std::vector<double> mask(num_slots_, 0.0);
        mask[i] = 1.0;
        Plaintext mask_pt = cc_->MakeCKKSPackedPlaintext(mask);
        auto masked = cc_->EvalMult(scalar_cts[i], mask_pt);

        if (i == 0) {
            result = masked;
        } else {
            result = cc_->EvalAdd(result, masked);
        }
    }
    return result;
}

std::vector<Ciphertext<DCRTPoly>> Server::matmul(
    const std::vector<Ciphertext<DCRTPoly>>& enc_C,
    const std::vector<Ciphertext<DCRTPoly>>& enc_V,
    int d) const
{
    int p = static_cast<int>(enc_V.size());
    std::vector<Ciphertext<DCRTPoly>> result(d * p);

    for (int i = 0; i < d; ++i) {
        for (int j = 0; j < p; ++j) {
            result[i * p + j] = innerProduct(enc_C[i], enc_V[j], d);
        }
    }
    return result;
}

Server::LanczosResult Server::lanczosIteration(
    const std::vector<Ciphertext<DCRTPoly>>& enc_C,
    const std::vector<Ciphertext<DCRTPoly>>& enc_V1,
    int d,
    int p,
    int m_iter,
    double eigenvalue_guess,
    int newton_iters,
    const std::vector<double>& asor_k_factors,
    LanczosStats* stats_out)
{
    if (p != 1) {
        throw std::invalid_argument("lanczosIteration: 仅支持 p=1");
    }
    if (m_iter < 1) {
        throw std::invalid_argument("lanczosIteration: m_iter 至少为 1");
    }

    const bool use_asor = !asor_k_factors.empty();

    // 单次 Lanczos 迭代（包含 Newton）大致消耗的乘法层数估算：
    //   innerProduct (1) + packScalars mask mult (1) + scalarVecMultiply*2 (2)
    //   + innerProduct(W,W) (1) + Newton(newton_iters * 3) + innerProduct(V,W) (1) ≈ 6 + 3*n
    // 为保险，Bootstrap 阈值取：单轮所需最小深度 + 余量
    const int newton_steps_used = use_asor
        ? static_cast<int>(asor_k_factors.size())
        : newton_iters;
    const uint32_t per_iter_depth =
        static_cast<uint32_t>(6 + 3 * std::max(newton_steps_used, 1));
    const uint32_t bootstrap_threshold = per_iter_depth + 2;

    LanczosResult out;
    out.alphas.reserve(m_iter);
    out.betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));
    out.V_all.reserve(m_iter);

    Ciphertext<DCRTPoly> V = enc_V1[0];
    Ciphertext<DCRTPoly> V_prev;
    Ciphertext<DCRTPoly> beta_prev_ct;
    const double guess_inv_sqrt =
        1.0 / std::sqrt(std::max(eigenvalue_guess, 1e-9));

    for (int iter = 0; iter < m_iter; ++iter) {
        if (bootstrap_enabled_ && iter > 0) {
            bootstrapIfNeeded(V, bootstrap_threshold, stats_out);
            if (iter > 0 && V_prev) {
                bootstrapIfNeeded(V_prev, bootstrap_threshold, stats_out);
            }
            if (beta_prev_ct) {
                bootstrapIfNeeded(beta_prev_ct, bootstrap_threshold, stats_out);
            }
        }

        out.V_all.push_back(V);

        // Step 1: W = C · V  （逐行 innerProduct 后 pack）
        std::vector<Ciphertext<DCRTPoly>> scalars(d);
        for (int i = 0; i < d; ++i) {
            scalars[i] = innerProduct(enc_C[i], V, d);
        }
        Ciphertext<DCRTPoly> Wvec = packScalarsToVector(scalars, d);

        // Step 1b: W = W − β_{j-1} · v_{j-1}
        if (iter > 0) {
            auto bv = scalarVecMultiply(beta_prev_ct, V_prev);
            Wvec = cc_->EvalSub(Wvec, bv);
        }

        // Step 2: α = V^T · W
        auto alpha = innerProduct(V, Wvec, d);
        out.alphas.push_back(alpha);

        const bool is_last = (iter == m_iter - 1);
        if (!is_last) {
            // Step 3: W_new = W − α · V
            auto alphaV = scalarVecMultiply(alpha, V);
            auto Wnew = cc_->EvalSub(Wvec, alphaV);

            // Step 4: β = ||W_new|| via Newton 1/√x
            auto norm_sq = innerProduct(Wnew, Wnew, d);
            InvSqrtResult inv = use_asor
                ? newton_->computeWithASOR(norm_sq, guess_inv_sqrt, asor_k_factors)
                : newton_->compute(norm_sq, guess_inv_sqrt, newton_iters);
            out.betas.push_back(inv.sqrt_val);

            // Step 5: v_{j+1} = W_new * (1/β)
            V_prev = V;
            beta_prev_ct = inv.sqrt_val;
            auto Vnext = scalarVecMultiply(inv.inv_sqrt, Wnew);
            V = std::move(Vnext);
        }
    }

    // 返回前对所有 α/β 做一次 Bootstrap 刷新，确保 Client 解密时
    // CKKS 近似误差检查（logApproxError）不触发——低秩数据下最后几个
    // α/β 的真值接近 0，噪声相对值较大时会被 OpenFHE 判为精度不足
    if (bootstrap_enabled_) {
        for (auto& ct : out.alphas) {
            if (remainingDepth(ct) < 10) {
                bootstrapIfNeeded(ct, 10, stats_out);
            }
        }
        for (auto& ct : out.betas) {
            if (remainingDepth(ct) < 10) {
                bootstrapIfNeeded(ct, 10, stats_out);
            }
        }
    }

    return out;
}
