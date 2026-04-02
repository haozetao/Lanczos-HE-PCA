#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <seal/seal.h>
#include <seal/util/common.h>

// CKKS：用 multiply_plain(全 1) 将 moving.scale 向 ref.scale 靠拢。
// 单步因子过大时 encode / 乘后 scale 会触发 SEAL scale out of bounds，故分多步；
// 且不可对明文做长链 mod_switch（plain.scale 不随层缩小）。
inline void ckks_adjust_scale_to_ref_loop(
    const std::shared_ptr<seal::SEALContext>& context,
    seal::Evaluator& evaluator,
    const seal::CKKSEncoder& encoder,
    seal::Ciphertext& moving,
    const seal::Ciphertext& ref)
{
    auto cd = context->get_context_data(moving.parms_id());
    if (!cd) {
        throw std::invalid_argument("ckks_adjust_scale_to_ref_loop: 无效 parms_id");
    }
    // 留出余量：SEAL 用 int(log2(scale)) 与 int(log2(scale))+1 做边界判断，贴边易越界
    const int B = static_cast<int>(cd->total_coeff_modulus_bit_count());
    const int B_eff = std::max(48, B - 20);
    std::vector<double> ones(encoder.slot_count(), 1.0);

    constexpr int kMaxIters = 256;
    for (int it = 0; it < kMaxIters; ++it) {
        if (seal::util::are_close(moving.scale(), ref.scale())) {
            return;
        }
        const double sm = moving.scale();
        const double sr = ref.scale();
        if (sm <= 0 || sr <= 0 || !std::isfinite(sm) || !std::isfinite(sr)) {
            throw std::invalid_argument("ckks_adjust_scale_to_ref_loop: 无效 scale");
        }
        const double ratio = sr / sm;
        if (seal::util::are_close(ratio, 1.0)) {
            return;
        }

        auto log2_pos = [](double x) {
            return std::log2(std::max(x, std::numeric_limits<double>::min()));
        };
        const double log2_sm = log2_pos(sm);
        const double log2_ratio = std::log2(ratio);

        // 每步在 log 域走一半（几何平均），避免一步 f=ratio 触发 encode / 乘后越界
        const double max_log2_f_ct =
            static_cast<double>(B_eff) - log2_sm - 6.0;
        const double max_log2_f_enc = static_cast<double>(B_eff) - 12.0;
        double cap = std::min(max_log2_f_ct, max_log2_f_enc);
        cap = std::max(0.25, cap);

        double log2_f = 0.5 * log2_ratio;
        if (log2_f > cap) {
            log2_f = cap;
        }
        if (log2_f < -cap) {
            log2_f = -cap;
        }

        double f = std::pow(2.0, log2_f);
        if (ratio >= 1.0 && f > ratio) {
            f = ratio;
        }
        if (ratio < 1.0 && f < ratio) {
            f = ratio;
        }

        if (!std::isfinite(f) || f <= 0) {
            throw std::invalid_argument("ckks_adjust_scale_to_ref_loop: 无效调整因子");
        }
        if (std::abs(f - 1.0) < 1e-15) {
            f = ratio;
        }

        seal::Plaintext pt;
        encoder.encode(ones, moving.parms_id(), f, pt);
        evaluator.multiply_plain_inplace(moving, pt);
    }

    if (!seal::util::are_close(moving.scale(), ref.scale())) {
        throw std::invalid_argument("ckks_adjust_scale_to_ref_loop: 未能在迭代内对齐 scale");
    }
}

// CKKS square 会把 scale 变为 scale²；在链深处若 int(log2(scale²)) ≥ B 会抛 scale out of bounds。
// 用 multiply_plain(全 1、明文槽值为 1) 且 scale<1 可压低密文 scale，且不改变解密后的槽内数值。
inline void ckks_shrink_scale_for_square_inplace(
    const std::shared_ptr<seal::SEALContext>& context,
    seal::Evaluator& evaluator,
    const seal::CKKSEncoder& encoder,
    seal::Ciphertext& ct)
{
    std::vector<double> ones(encoder.slot_count(), 1.0);
    constexpr int kMaxShrink = 96;
    for (int t = 0; t < kMaxShrink; ++t) {
        auto cd = context->get_context_data(ct.parms_id());
        if (!cd) {
            throw std::invalid_argument("ckks_shrink_scale_for_square_inplace: 无效 parms_id");
        }
        const int B = static_cast<int>(cd->total_coeff_modulus_bit_count());
        const double s = ct.scale();
        const double log2s = std::log2(std::max(s, std::numeric_limits<double>::min()));
        if (2.0 * log2s < static_cast<double>(B) - 4.0) {
            return;
        }
        seal::Plaintext pt;
        encoder.encode(ones, ct.parms_id(), 0.5, pt);
        evaluator.multiply_plain_inplace(ct, pt);
    }
    throw std::invalid_argument("ckks_shrink_scale_for_square_inplace: 无法在 square 前将 scale 压到安全范围");
}

inline void ckks_multiply_plain_ones_factor_inplace(
    const std::shared_ptr<seal::SEALContext>& context,
    seal::Evaluator& evaluator,
    const seal::CKKSEncoder& encoder,
    seal::Ciphertext& ct,
    double factor)
{
    (void)context;
    std::vector<double> ones(encoder.slot_count(), 1.0);
    seal::Plaintext pt;
    encoder.encode(ones, ct.parms_id(), factor, pt);
    evaluator.multiply_plain_inplace(ct, pt);
}

// multiply 后 scale ≈ s1*s2；在链深处可能越界。对**两个**操作数同乘 sqrt(0.5)（槽值仍为 1），
// 使 s1*s2 每次约乘 0.5 且两 scale 仍相等（在已与 are_close 对齐的前提下）。
inline void ckks_prepare_binary_multiply_inplace(
    const std::shared_ptr<seal::SEALContext>& context,
    seal::Evaluator& evaluator,
    const seal::CKKSEncoder& encoder,
    seal::Ciphertext& a,
    seal::Ciphertext& b)
{
    constexpr int kMax = 128;
    const double sqrt_half = std::sqrt(0.5);
    for (int t = 0; t < kMax; ++t) {
        auto cd = context->get_context_data(a.parms_id());
        if (!cd) {
            throw std::invalid_argument("ckks_prepare_binary_multiply_inplace: 无效 parms_id");
        }
        const int B = static_cast<int>(cd->total_coeff_modulus_bit_count());
        const double sa = a.scale();
        const double sb = b.scale();
        const double la = std::log2(std::max(sa, std::numeric_limits<double>::min()));
        const double lb = std::log2(std::max(sb, std::numeric_limits<double>::min()));
        if (la + lb < static_cast<double>(B) - 4.0) {
            return;
        }
        ckks_multiply_plain_ones_factor_inplace(context, evaluator, encoder, a, sqrt_half);
        ckks_multiply_plain_ones_factor_inplace(context, evaluator, encoder, b, sqrt_half);
    }
    throw std::invalid_argument("ckks_prepare_binary_multiply_inplace: 无法将 scale 积压到安全范围");
}
