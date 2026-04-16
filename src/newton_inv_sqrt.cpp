#include "newton_inv_sqrt.h"

#include <cmath>
#include <stdexcept>

#include "ckks_scale_align.h"

NewtonInvSqrt::NewtonInvSqrt(
    std::shared_ptr<seal::SEALContext> context,
    seal::Evaluator& evaluator,
    seal::CKKSEncoder& encoder,
    seal::Encryptor& encryptor,
    const seal::RelinKeys& relin_keys,
    double scale)
    : context_(std::move(context))
    , evaluator_(evaluator)
    , encoder_(encoder)
    , encryptor_(encryptor)
    , relin_keys_(relin_keys)
    , scale_(scale)
{
}

void NewtonInvSqrt::align_two(
    seal::Evaluator& ev,
    const seal::SEALContext& ctx,
    seal::Ciphertext& a,
    seal::Ciphertext& b)
{
    if (a.parms_id() != b.parms_id()) {
        auto da = ctx.get_context_data(a.parms_id());
        auto db = ctx.get_context_data(b.parms_id());
        if (!da || !db) {
            throw std::invalid_argument("NewtonInvSqrt::align_two: invalid parms_id");
        }
        std::size_t ia = da->chain_index();
        std::size_t ib = db->chain_index();
        if (ia > ib) {
            ev.mod_switch_to_inplace(a, b.parms_id());
        } else if (ib > ia) {
            ev.mod_switch_to_inplace(b, a.parms_id());
        } else {
            ev.mod_switch_to_inplace(a, b.parms_id());
        }
    }
}

void NewtonInvSqrt::adjust_scale_to_ref_inplace_(
    seal::Ciphertext& moving, const seal::Ciphertext& ref) const
{
    if (moving.parms_id() != ref.parms_id()) {
        throw std::invalid_argument("NewtonInvSqrt::adjust_scale_to_ref_inplace_: parms mismatch");
    }
    ckks_adjust_scale_to_ref_loop(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, moving, ref);
}

seal::Ciphertext NewtonInvSqrt::newtonStep(
    const seal::Ciphertext& x_ct,
    seal::Ciphertext& y,
    double half_factor) const
{
    // y^2
    ckks_shrink_scale_for_square_inplace(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, y);
    seal::Ciphertext y_sq;
    evaluator_.square(y, y_sq);
    evaluator_.relinearize_inplace(y_sq, relin_keys_);
    evaluator_.rescale_to_next_inplace(y_sq);

    // x * y^2
    seal::Ciphertext x_adj = x_ct;
    seal::Ciphertext y_sq_m = y_sq;
    align_two(evaluator_, *context_, x_adj, y_sq_m);
    adjust_scale_to_ref_inplace_(y_sq_m, x_adj);
    ckks_prepare_binary_multiply_inplace(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, x_adj, y_sq_m);
    seal::Ciphertext x_y_sq;
    evaluator_.multiply(x_adj, y_sq_m, x_y_sq);
    evaluator_.relinearize_inplace(x_y_sq, relin_keys_);
    evaluator_.rescale_to_next_inplace(x_y_sq);

    // t = 3 - x*y^2
    seal::Ciphertext neg_xysq;
    evaluator_.negate(x_y_sq, neg_xysq);
    seal::Plaintext three_pt;
    encoder_.encode(3.0, neg_xysq.scale(), three_pt);
    evaluator_.mod_switch_to_inplace(three_pt, neg_xysq.parms_id());
    seal::Ciphertext t;
    evaluator_.add_plain(neg_xysq, three_pt, t);

    // y * t
    seal::Ciphertext y_m = y;
    seal::Ciphertext t_m = t;
    align_two(evaluator_, *context_, y_m, t_m);
    adjust_scale_to_ref_inplace_(t_m, y_m);
    ckks_prepare_binary_multiply_inplace(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, y_m, t_m);
    seal::Ciphertext prod;
    evaluator_.multiply(y_m, t_m, prod);
    evaluator_.relinearize_inplace(prod, relin_keys_);
    evaluator_.rescale_to_next_inplace(prod);

    // * half_factor (0.5 for standard, k_{i+1}/2 for aSOR)
    seal::Plaintext hf_pt;
    encoder_.encode(half_factor, prod.scale(), hf_pt);
    evaluator_.mod_switch_to_inplace(hf_pt, prod.parms_id());
    evaluator_.multiply_plain_inplace(prod, hf_pt);
    evaluator_.rescale_to_next_inplace(prod);

    return prod;
}

InvSqrtResult NewtonInvSqrt::compute(
    const seal::Ciphertext& x_ct,
    double initial_guess,
    int iterations) const
{
    seal::Plaintext y0_pt;
    encoder_.encode(initial_guess, scale_, y0_pt);
    seal::Ciphertext y;
    encryptor_.encrypt(y0_pt, y);

    for (int it = 0; it < iterations; ++it) {
        y = newtonStep(x_ct, y, 0.5);
    }

    // sqrt(x) = x * (1/sqrt(x))
    seal::Ciphertext x_adj = x_ct;
    seal::Ciphertext y_m = y;
    align_two(evaluator_, *context_, x_adj, y_m);
    adjust_scale_to_ref_inplace_(y_m, x_adj);
    ckks_prepare_binary_multiply_inplace(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, x_adj, y_m);
    seal::Ciphertext sqrt_val;
    evaluator_.multiply(x_adj, y_m, sqrt_val);
    evaluator_.relinearize_inplace(sqrt_val, relin_keys_);
    evaluator_.rescale_to_next_inplace(sqrt_val);

    return InvSqrtResult{std::move(y), std::move(sqrt_val)};
}

InvSqrtResult NewtonInvSqrt::computeWithASOR(
    const seal::Ciphertext& x_ct,
    double initial_guess,
    const std::vector<double>& k_factors) const
{
    const int n = static_cast<int>(k_factors.size());
    if (n < 1) {
        throw std::invalid_argument("computeWithASOR: k_factors must not be empty");
    }

    // Absorb k_1 into initial guess: encrypt(k_1 * initial_guess)
    const double scaled_guess = k_factors[0] * initial_guess;
    seal::Plaintext y0_pt;
    encoder_.encode(scaled_guess, scale_, y0_pt);
    seal::Ciphertext y;
    encryptor_.encrypt(y0_pt, y);

    for (int it = 0; it < n; ++it) {
        // half_factor = k_{i+1}/2 for intermediate steps, 0.5 for last step
        double hf;
        if (it < n - 1) {
            hf = k_factors[it + 1] / 2.0;
        } else {
            hf = 0.5;
        }
        y = newtonStep(x_ct, y, hf);
    }

    // sqrt(x) = x * (1/sqrt(x))
    seal::Ciphertext x_adj = x_ct;
    seal::Ciphertext y_m = y;
    align_two(evaluator_, *context_, x_adj, y_m);
    adjust_scale_to_ref_inplace_(y_m, x_adj);
    ckks_prepare_binary_multiply_inplace(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, x_adj, y_m);
    seal::Ciphertext sqrt_val;
    evaluator_.multiply(x_adj, y_m, sqrt_val);
    evaluator_.relinearize_inplace(sqrt_val, relin_keys_);
    evaluator_.rescale_to_next_inplace(sqrt_val);

    return InvSqrtResult{std::move(y), std::move(sqrt_val)};
}
