#include "server.h"

#include <cmath>
#include <stdexcept>

#include "ckks_scale_align.h"
#include "newton_inv_sqrt.h"

void Server::align_two_inplace(seal::Ciphertext& a, seal::Ciphertext& b) const
{
    if (a.parms_id() == b.parms_id()) {
        return;
    }
    auto da = context_->get_context_data(a.parms_id());
    auto db = context_->get_context_data(b.parms_id());
    if (!da || !db) {
        throw std::invalid_argument("align_two_inplace: 无效 parms_id");
    }
    std::size_t ia = da->chain_index();
    std::size_t ib = db->chain_index();
    // SEAL: chain_index 越大越接近“顶层”（未消耗层级越多）；只能向下 mod_switch
    if (ia > ib) {
        evaluator_.mod_switch_to_inplace(a, b.parms_id());
    } else if (ib > ia) {
        evaluator_.mod_switch_to_inplace(b, a.parms_id());
    } else {
        evaluator_.mod_switch_to_inplace(a, b.parms_id());
    }
}

void Server::adjust_ckks_scale_to_ref_inplace_(
    seal::Ciphertext& moving, const seal::Ciphertext& ref) const
{
    if (moving.parms_id() != ref.parms_id()) {
        throw std::invalid_argument("adjust_ckks_scale_to_ref_inplace_: parms 不一致");
    }
    ckks_adjust_scale_to_ref_loop(
        context_, const_cast<seal::Evaluator&>(evaluator_), encoder_, moving, ref);
}

seal::Ciphertext Server::innerProduct(
    const seal::Ciphertext& a,
    const seal::Ciphertext& b,
    int dim) const
{
    seal::Ciphertext a1(a);
    seal::Ciphertext b1(b);
    align_two_inplace(a1, b1);
    adjust_ckks_scale_to_ref_inplace_(b1, a1);

    seal::Ciphertext product;
    evaluator_.multiply(a1, b1, product);
    evaluator_.relinearize_inplace(product, relin_keys_);
    evaluator_.rescale_to_next_inplace(product);

    seal::Ciphertext rotated;
    for (int step = dim / 2; step >= 1; step /= 2) {
        evaluator_.rotate_vector(product, step, galois_keys_, rotated);
        adjust_ckks_scale_to_ref_inplace_(rotated, product);
        evaluator_.add_inplace(product, rotated);
    }
    return product;
}

seal::Ciphertext Server::broadcastScalar(
    const seal::Ciphertext& scalar_ct, int dim) const
{
    seal::Ciphertext result = scalar_ct;
    for (int k = 1; k < dim; ++k) {
        seal::Ciphertext rot;
        evaluator_.rotate_vector(scalar_ct, k, galois_keys_, rot);
        align_two_inplace(rot, result);
        adjust_ckks_scale_to_ref_inplace_(rot, result);
        evaluator_.add_inplace(result, rot);
    }
    return result;
}

seal::Ciphertext Server::scalarVecMultiply(
    const seal::Ciphertext& scalar_ct,
    const seal::Ciphertext& vec_ct,
    int dim) const
{
    seal::Ciphertext b = broadcastScalar(scalar_ct, dim);
    seal::Ciphertext v = vec_ct;
    align_two_inplace(b, v);
    adjust_ckks_scale_to_ref_inplace_(v, b);
    seal::Ciphertext out;
    evaluator_.multiply(b, v, out);
    evaluator_.relinearize_inplace(out, relin_keys_);
    evaluator_.rescale_to_next_inplace(out);
    return out;
}

seal::Ciphertext Server::packScalarsToVector(
    const std::vector<seal::Ciphertext>& scalar_cts, int d) const
{
    seal::Ciphertext result = scalar_cts[0];
    for (int i = 1; i < d; ++i) {
        seal::Ciphertext si = scalar_cts[i];
        align_two_inplace(si, result);
        seal::Ciphertext rot;
        evaluator_.rotate_vector(si, i, galois_keys_, rot);
        align_two_inplace(rot, result);
        adjust_ckks_scale_to_ref_inplace_(rot, result);
        evaluator_.add_inplace(result, rot);
    }
    return result;
}

Server::Server(std::shared_ptr<seal::SEALContext> ctx,
               const seal::PublicKey& pk,
               const seal::RelinKeys& rlk,
               const seal::GaloisKeys& glk,
               double ckks_scale)
    : context_(std::move(ctx))
    , evaluator_(*context_)
    , encoder_(*context_)
    , encryptor_(*context_, pk)
    , relin_keys_(rlk)
    , galois_keys_(glk)
    , scale_(ckks_scale)
{
    newton_ = std::make_unique<NewtonInvSqrt>(
        context_, evaluator_, encoder_, encryptor_, relin_keys_, scale_);
}

std::vector<seal::Ciphertext> Server::matmul(
    const std::vector<seal::Ciphertext>& enc_C,
    const std::vector<seal::Ciphertext>& enc_V,
    int d) const
{
    int p = static_cast<int>(enc_V.size());
    std::vector<seal::Ciphertext> result(d * p);

    for (int i = 0; i < d; ++i) {
        for (int j = 0; j < p; ++j) {
            seal::Ciphertext Ci(enc_C[i]);
            seal::Ciphertext Vj(enc_V[j]);
            align_two_inplace(Ci, Vj);
            result[i * p + j] = innerProduct(Ci, Vj, d);
        }
    }
    return result;
}

Server::LanczosResult Server::lanczosIteration(
    const std::vector<seal::Ciphertext>& enc_C,
    const std::vector<seal::Ciphertext>& enc_V1,
    int d,
    int p,
    int m_iter,
    double eigenvalue_guess,
    int newton_iters)
{
    if (p != 1) {
        throw std::invalid_argument("lanczosIteration: 当前实现仅支持 p=1");
    }
    if (m_iter < 1) {
        throw std::invalid_argument("lanczosIteration: m_iter 至少为 1");
    }

    LanczosResult out;
    out.alphas.reserve(m_iter);
    out.betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));

    seal::Ciphertext V = enc_V1[0];
    seal::Ciphertext V_prev;
    seal::Ciphertext beta_prev_ct;
    const double guess_inv_sqrt =
        1.0 / std::sqrt(std::max(eigenvalue_guess, 1e-9));

    for (int iter = 0; iter < m_iter; ++iter) {
        // Step 1: W = C · V
        std::vector<seal::Ciphertext> scalars(d);
        for (int i = 0; i < d; ++i) {
            seal::Ciphertext Ci(enc_C[i]);
            seal::Ciphertext Vc(V);
            align_two_inplace(Ci, Vc);
            scalars[i] = innerProduct(Ci, Vc, d);
        }
        seal::Ciphertext Wvec = packScalarsToVector(scalars, d);

        // Step 1b: W = W − β_{j-1} · v_{j-1}（三项递推关键项）
        if (iter > 0) {
            seal::Ciphertext bv = scalarVecMultiply(beta_prev_ct, V_prev, d);
            align_two_inplace(Wvec, bv);
            adjust_ckks_scale_to_ref_inplace_(bv, Wvec);
            evaluator_.sub_inplace(Wvec, bv);
        }

        // Step 2: α = V^T · W
        seal::Ciphertext V_for_ip = V;
        seal::Ciphertext W_for_ip = Wvec;
        align_two_inplace(V_for_ip, W_for_ip);
        seal::Ciphertext alpha = innerProduct(V_for_ip, W_for_ip, d);
        out.alphas.push_back(alpha);

        // Step 3: W_new = W − α · V
        seal::Ciphertext alphaV = scalarVecMultiply(alpha, V, d);
        seal::Ciphertext W_sub = Wvec;
        seal::Ciphertext av_sub = alphaV;
        align_two_inplace(W_sub, av_sub);
        adjust_ckks_scale_to_ref_inplace_(av_sub, W_sub);
        seal::Ciphertext Wnew;
        evaluator_.sub(W_sub, av_sub, Wnew);

        // Step 4: β = ‖W_new‖ via Newton 1/√x
        seal::Ciphertext norm_sq = innerProduct(Wnew, Wnew, d);
        InvSqrtResult inv = newton_->compute(norm_sq, guess_inv_sqrt, newton_iters);

        if (iter < m_iter - 1) {
            out.betas.push_back(inv.sqrt_val);
        }

        // Step 5: v_{j+1} = W_new / β，保留 v_j 和 β_j 供下一轮三项递推
        V_prev = V;
        beta_prev_ct = inv.sqrt_val;
        seal::Ciphertext Vnext = scalarVecMultiply(inv.inv_sqrt, Wnew, d);
        V = std::move(Vnext);
    }

    return out;
}
