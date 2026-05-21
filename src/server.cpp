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

Ciphertext<DCRTPoly> Server::innerProductReplicated(
    const Ciphertext<DCRTPoly>& a,
    const Ciphertext<DCRTPoly>& b,
    int d) const
{
    if (d <= 0) {
        throw std::invalid_argument("innerProductReplicated: d 必须 > 0");
    }
    if (d & (d - 1)) {
        throw std::invalid_argument(
            "innerProductReplicated: d=" + std::to_string(d)
            + " 暂仅支持 2 的幂");
    }
    // a, b 是 replicated 向量：slot[i] = vec[i mod d]
    // 逐元素乘后用 d-step rotate-and-sum：在 log2(d) 次旋转后，前 d 个 slot
    // 内每个 slot 都等于 Σ_{i=0..d-1} a_i * b_i。由于 a, b 在 [d, 2d) 段
    // 也是相同向量的副本，cyclic-rotate by num_slots 对前 d slot 的求和
    // 等价于 cyclic-rotate by d (因 d | num_slots)，故 log2(d) 步刚好覆盖。
    auto product = cc_->EvalMult(a, b);
    for (int step = 1; step < d; step *= 2) {
        auto rotated = cc_->EvalRotate(product, step);
        product = cc_->EvalAdd(product, rotated);
    }
    return product;
}

Ciphertext<DCRTPoly> Server::matvecDiagonal(
    const std::vector<Ciphertext<DCRTPoly>>& enc_C_diag,
    const Ciphertext<DCRTPoly>& v_ct,
    int d,
    int bsgs_b) const
{
    if (d <= 0) {
        throw std::invalid_argument("matvecDiagonal: d 必须 > 0");
    }
    if (static_cast<int>(enc_C_diag.size()) != d) {
        throw std::invalid_argument(
            "matvecDiagonal: enc_C_diag 大小 " + std::to_string(enc_C_diag.size())
            + " 与 d=" + std::to_string(d) + " 不一致");
    }
    if (bsgs_b < 0 || (bsgs_b > 0 && d % bsgs_b != 0)) {
        throw std::invalid_argument(
            "matvecDiagonal: bsgs_b=" + std::to_string(bsgs_b)
            + " 必须为 0 或 d 的因子");
    }

    // ── Plain Diagonal: result = Σ_{k=0..d-1} diag_k ⊙ Rot(v, k) ──
    if (bsgs_b == 0) {
        Ciphertext<DCRTPoly> result;
        for (int k = 0; k < d; ++k) {
            Ciphertext<DCRTPoly> vrot = (k == 0) ? v_ct : cc_->EvalRotate(v_ct, k);
            auto term = cc_->EvalMult(enc_C_diag[static_cast<size_t>(k)], vrot);
            if (k == 0) {
                result = term;
            } else {
                result = cc_->EvalAdd(result, term);
            }
        }
        return result;
    }

    // ── BSGS: k = b·i + j, i ∈ [0, a), j ∈ [0, b), ab = d ─────────────────
    //   Cv = Σ_i Rot( Σ_j diag'_{bi+j} ⊙ Rot(v, j), b·i )
    //   diag'_{bi+j} = Rot(diag_{bi+j}, -b·i)  ← Client 已预算
    const int b = bsgs_b;
    const int a = d / b;

    // Baby steps: 预算 b 个 Rot(v, j)
    std::vector<Ciphertext<DCRTPoly>> v_baby(static_cast<size_t>(b));
    v_baby[0] = v_ct;
    for (int j = 1; j < b; ++j) {
        v_baby[static_cast<size_t>(j)] = cc_->EvalRotate(v_ct, j);
    }

    Ciphertext<DCRTPoly> result;
    for (int i = 0; i < a; ++i) {
        // 内 sum: accumulator = Σ_j diag'_{bi+j} ⊙ v_baby[j]
        Ciphertext<DCRTPoly> accumulator;
        for (int j = 0; j < b; ++j) {
            int k = i * b + j;
            auto term = cc_->EvalMult(
                enc_C_diag[static_cast<size_t>(k)],
                v_baby[static_cast<size_t>(j)]);
            if (j == 0) {
                accumulator = term;
            } else {
                accumulator = cc_->EvalAdd(accumulator, term);
            }
        }
        // Giant step: 把 accumulator cyclic-rotate by b·i 回到正确位置
        Ciphertext<DCRTPoly> shifted = (i == 0) ? accumulator
                                                : cc_->EvalRotate(accumulator, i * b);
        if (i == 0) {
            result = shifted;
        } else {
            result = cc_->EvalAdd(result, shifted);
        }
    }
    return result;
}

Ciphertext<DCRTPoly> Server::scalarVecMultiply(
    const Ciphertext<DCRTPoly>& scalar_ct,
    const Ciphertext<DCRTPoly>& vec_ct) const
{
    // scalar_ct 来自 innerProductReplicated，已全 slot 填充 → 直接逐元素乘
    return cc_->EvalMult(scalar_ct, vec_ct);
}

Server::LanczosResult Server::lanczosIteration(
    const std::vector<Ciphertext<DCRTPoly>>& enc_C_diag,
    const Ciphertext<DCRTPoly>& v1_ct,
    int d,
    int m_iter,
    double eigenvalue_guess,
    const std::vector<double>& per_iter_eigenvalue_guesses,
    int newton_iters,
    int bsgs_b,
    const std::vector<double>& asor_k_factors,
    LanczosStats* stats_out,
    bool enable_fro,
    int fro_skip_first)
{
    if (m_iter < 1) {
        throw std::invalid_argument("lanczosIteration: m_iter 至少为 1");
    }
    if (static_cast<int>(enc_C_diag.size()) != d) {
        throw std::invalid_argument(
            "lanczosIteration: enc_C_diag 大小 " + std::to_string(enc_C_diag.size())
            + " 与 d=" + std::to_string(d) + " 不一致");
    }

    const bool use_asor = !asor_k_factors.empty();

    // 单次 Lanczos 迭代（包含 Newton）消耗的乘法层数估算：
    //   depth_probe.cpp 对 OpenFHE CKKS FLEXIBLEAUTO 的实测为
    //   V -> V_next critical path = 4 + 4 * NewtonSteps（无 FRO）。
    //   开启 FRO 后每次 reorth 步在 W_new 上额外消耗 2 层，
    //   最坏迭代（iter = m-1）消耗 2 * (m-1) 层。
    const int newton_steps_used = use_asor
        ? static_cast<int>(asor_k_factors.size())
        : newton_iters;
    const int fro_skip = std::max(0, fro_skip_first);
    const int max_reorth_steps = enable_fro
        ? std::max(0, m_iter - 1 - fro_skip)
        : 0;
    const uint32_t per_iter_depth = static_cast<uint32_t>(
        4 + 4 * std::max(newton_steps_used, 1) + 2 * max_reorth_steps);
    const uint32_t bootstrap_threshold = per_iter_depth + 2;

    LanczosResult out;
    out.alphas.reserve(m_iter);
    out.betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));
    out.V_all.reserve(m_iter);

    Ciphertext<DCRTPoly> V = v1_ct;
    Ciphertext<DCRTPoly> V_prev;
    Ciphertext<DCRTPoly> beta_prev_ct;
    const double fallback_guess =
        std::max(eigenvalue_guess, 1e-18);

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

        // Step 1: W = C · V  （Diagonal/BSGS matvec，直接输出 replicated 向量）
        Ciphertext<DCRTPoly> Wvec = matvecDiagonal(enc_C_diag, V, d, bsgs_b);

        // Step 1b: W = W − β_{j-1} · v_{j-1}
        if (iter > 0) {
            auto bv = scalarVecMultiply(beta_prev_ct, V_prev);
            Wvec = cc_->EvalSub(Wvec, bv);
        }

        // Step 2: α = V^T · W   (replicated 内积，log d 步 rotate)
        auto alpha = innerProductReplicated(V, Wvec, d);
        out.alphas.push_back(alpha);

        const bool is_last = (iter == m_iter - 1);
        if (!is_last) {
            // Step 3: W_new = W − α · V
            auto alphaV = scalarVecMultiply(alpha, V);
            auto Wnew = cc_->EvalSub(Wvec, alphaV);

            // Step 3b (Optional): Full Reorthogonalization (HE-FRO)
            //   W_new ← W_new − Σ_{k=0..iter-1} (V_k · W_new) · V_k
            //   防止 Lanczos 向量正交性丢失导致 Krylov 子空间坍缩。
            //   - 跳过前 fro_skip 步（j=0..fro_skip-1）：那时正交性还没坏，
            //     省下大约 (fro_skip * 2) 层关键深度。
            //   - V_all[k] 是 BS 后入库的 fresh 密文，不需要在 reorth 期间
            //     再次 BS，因为它只作为乘法的"另一边"，level 不会被消耗。
            if (enable_fro && iter >= fro_skip) {
                for (int k = 0; k < iter; ++k) {
                    const auto& Vk = out.V_all[k];
                    auto proj_k = innerProductReplicated(Vk, Wnew, d);
                    auto sub_k = scalarVecMultiply(proj_k, Vk);
                    Wnew = cc_->EvalSub(Wnew, sub_k);
                }
            }

            // Step 4: β = ||W_new|| via Newton 1/√x
            auto norm_sq = innerProductReplicated(Wnew, Wnew, d);
            const double iter_guess =
                (static_cast<size_t>(iter) < per_iter_eigenvalue_guesses.size())
                    ? per_iter_eigenvalue_guesses[static_cast<size_t>(iter)]
                    : fallback_guess;
            const double guess_inv_sqrt =
                1.0 / std::sqrt(std::max(iter_guess, 1e-18));
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
