#include "client.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

std::vector<int> galoisStepsForDim(int d, int poly_modulus_degree)
{
    std::vector<int> steps;
    for (int s = 1; s <= std::max(16, d + 6); ++s) {
        steps.push_back(s);
    }
    int half_slots = poly_modulus_degree / 2;
    for (int s = 1; s < half_slots; s *= 2) {
        steps.push_back(s);
    }
    std::sort(steps.begin(), steps.end());
    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
    return steps;
}

} // namespace

Client::Client(int d, int p, int m)
    : d_(d), p_(p), m_(m), scale_(std::pow(2.0, 40))
{
    // 方向2：poly=32768， coeff 链按 SEAL 安全上限自动填满 40-bit 素数
    seal::EncryptionParameters params(seal::scheme_type::ckks);
    params.set_poly_modulus_degree(32768);

    size_t max_bits = seal::CoeffModulus::MaxBitCount(
        32768, seal::sec_level_type::tc128);
    int n40 = static_cast<int>((max_bits - 120) / 40);
    if (n40 < 4) {
        n40 = 4;
    }
    while (static_cast<size_t>(60 + n40 * 40 + 60) > max_bits && n40 > 2) {
        --n40;
    }

    std::vector<int> bit_sizes;
    bit_sizes.push_back(60);
    for (int i = 0; i < n40; ++i) {
        bit_sizes.push_back(40);
    }
    bit_sizes.push_back(60);

    params.set_coeff_modulus(
        seal::CoeffModulus::Create(32768, bit_sizes));

    context_ = std::make_shared<seal::SEALContext>(params);

    seal::KeyGenerator keygen(*context_);
    secret_key_ = keygen.secret_key();
    keygen.create_public_key(public_key_);
    keygen.create_relin_keys(relin_keys_);

    std::vector<int> rot_steps = galoisStepsForDim(d_, 32768);
    keygen.create_galois_keys(rot_steps, galois_keys_);

    encoder_ = std::make_unique<seal::CKKSEncoder>(*context_);
    encryptor_ = std::make_unique<seal::Encryptor>(*context_, public_key_);
    decryptor_ = std::make_unique<seal::Decryptor>(*context_, secret_key_);
}

void Client::generateCovarianceMatrix()
{
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.0, 1.0);

    Eigen::MatrixXd A(d_, d_);
    for (int i = 0; i < d_; ++i) {
        for (int j = 0; j < d_; ++j) {
            A(i, j) = dist(rng);
        }
    }

    C_ = A.transpose() * A + Eigen::MatrixXd::Identity(d_, d_);
}

std::vector<seal::Ciphertext> Client::encryptCovMatrix() const
{
    size_t slot_count = encoder_->slot_count();
    std::vector<seal::Ciphertext> enc_C(d_);

    for (int i = 0; i < d_; ++i) {
        std::vector<double> row(slot_count, 0.0);
        for (int k = 0; k < d_; ++k) {
            row[k] = C_(i, k);
        }

        seal::Plaintext pt;
        encoder_->encode(row, scale_, pt);
        encryptor_->encrypt(pt, enc_C[i]);
    }
    return enc_C;
}

std::vector<seal::Ciphertext> Client::encryptBlockVec(
    const Eigen::MatrixXd& V) const
{
    size_t slot_count = encoder_->slot_count();
    int cols = static_cast<int>(V.cols());
    std::vector<seal::Ciphertext> enc_V(cols);

    for (int j = 0; j < cols; ++j) {
        std::vector<double> col(slot_count, 0.0);
        for (int i = 0; i < d_; ++i) {
            col[i] = V(i, j);
        }

        seal::Plaintext pt;
        encoder_->encode(col, scale_, pt);
        encryptor_->encrypt(pt, enc_V[j]);
    }
    return enc_V;
}

std::vector<seal::Ciphertext> Client::encryptColumnVector(
    const Eigen::VectorXd& v) const
{
    if (v.size() != d_) {
        throw std::invalid_argument("encryptColumnVector: 维度与 d 不一致");
    }
    Eigen::MatrixXd M(d_, 1);
    M.col(0) = v;
    return encryptBlockVec(M);
}

Eigen::MatrixXd Client::decryptToMatrix(
    const std::vector<seal::Ciphertext>& enc_W,
    int rows, int cols) const
{
    Eigen::MatrixXd result(rows, cols);

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            seal::Plaintext pt;
            decryptor_->decrypt(enc_W[i * cols + j], pt);

            std::vector<double> decoded;
            encoder_->decode(pt, decoded);

            result(i, j) = decoded[0];
        }
    }
    return result;
}

Eigen::MatrixXd Client::buildTridiagonalFromEncrypted(
    const std::vector<seal::Ciphertext>& enc_alphas,
    const std::vector<seal::Ciphertext>& enc_betas,
    int m) const
{
    if (static_cast<int>(enc_alphas.size()) != m) {
        throw std::invalid_argument("buildTridiagonalFromEncrypted: α 数量与 m 不符");
    }
    if (m > 1 && static_cast<int>(enc_betas.size()) != m - 1) {
        throw std::invalid_argument("buildTridiagonalFromEncrypted: β 数量应为 m-1");
    }

    std::vector<double> alphas(m);
    for (int i = 0; i < m; ++i) {
        seal::Plaintext pt;
        decryptor_->decrypt(enc_alphas[i], pt);
        std::vector<double> vals;
        encoder_->decode(pt, vals);
        alphas[i] = vals[0];
    }

    std::vector<double> betas;
    betas.reserve(m > 1 ? static_cast<size_t>(m - 1) : 0);
    for (int i = 0; i < m - 1; ++i) {
        seal::Plaintext pt;
        decryptor_->decrypt(enc_betas[i], pt);
        std::vector<double> vals;
        encoder_->decode(pt, vals);
        betas.push_back(vals[0]);
    }

    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(m, m);
    for (int i = 0; i < m; ++i) {
        T(i, i) = alphas[i];
        if (i < m - 1) {
            T(i, i + 1) = betas[i];
            T(i + 1, i) = betas[i];
        }
    }
    return T;
}

Eigen::MatrixXd Client::plaintextMirrorHeLanczosTridiagonal(
    const Eigen::VectorXd& v0,
    int m_iter) const
{
    if (v0.size() != d_) {
        throw std::invalid_argument("plaintextMirrorHeLanczosTridiagonal: v0 维度与 d 不一致");
    }
    if (m_iter < 1) {
        throw std::invalid_argument("plaintextMirrorHeLanczosTridiagonal: m_iter 至少为 1");
    }

    Eigen::VectorXd V = v0.normalized();
    Eigen::VectorXd V_prev = Eigen::VectorXd::Zero(d_);
    double beta_prev = 0.0;

    std::vector<double> alphas;
    std::vector<double> betas;
    alphas.reserve(static_cast<size_t>(m_iter));
    betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));

    for (int iter = 0; iter < m_iter; ++iter) {
        Eigen::VectorXd W = C_ * V - beta_prev * V_prev;
        const double alpha = V.dot(W);
        alphas.push_back(alpha);
        if (iter == m_iter - 1) {
            break;
        }
        Eigen::VectorXd Wnew = W - alpha * V;
        const double norm = Wnew.norm();
        if (norm < 1e-14) {
            break;
        }
        betas.push_back(norm);
        V_prev = V;
        V = Wnew / norm;
        beta_prev = norm;
    }

    const int m = static_cast<int>(alphas.size());
    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(m, m);
    for (int i = 0; i < m; ++i) {
        T(i, i) = alphas[static_cast<size_t>(i)];
        if (i < m - 1 && static_cast<size_t>(i) < betas.size()) {
            const double b = betas[static_cast<size_t>(i)];
            T(i, i + 1) = b;
            T(i + 1, i) = b;
        }
    }
    return T;
}

Eigen::MatrixXd Client::plaintextStandardLanczosTridiagonal(
    const Eigen::VectorXd& v0,
    int m_iter) const
{
    if (v0.size() != d_) {
        throw std::invalid_argument("plaintextStandardLanczosTridiagonal: v0 维度与 d 不一致");
    }
    if (m_iter < 1) {
        throw std::invalid_argument("plaintextStandardLanczosTridiagonal: m_iter 至少为 1");
    }

    Eigen::VectorXd v_prev = Eigen::VectorXd::Zero(d_);
    Eigen::VectorXd v = v0.normalized();
    double beta_prev = 0.0;

    std::vector<double> alphas;
    std::vector<double> betas;
    alphas.reserve(static_cast<size_t>(m_iter));
    betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));

    for (int j = 0; j < m_iter; ++j) {
        Eigen::VectorXd u = C_ * v;
        const double alpha = v.dot(u);
        alphas.push_back(alpha);
        Eigen::VectorXd w = u - alpha * v - beta_prev * v_prev;
        if (j == m_iter - 1) {
            break;
        }
        const double beta = w.norm();
        if (beta < 1e-14) {
            break;
        }
        betas.push_back(beta);
        v_prev = v;
        v = w / beta;
        beta_prev = beta;
    }

    const int m = static_cast<int>(alphas.size());
    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(m, m);
    for (int i = 0; i < m; ++i) {
        T(i, i) = alphas[static_cast<size_t>(i)];
        if (i < m - 1 && static_cast<size_t>(i) < betas.size()) {
            const double b = betas[static_cast<size_t>(i)];
            T(i, i + 1) = b;
            T(i + 1, i) = b;
        }
    }
    return T;
}

CWFilterResult Client::cullumWilloughbyFilter(
    const Eigen::MatrixXd& T_m,
    int K) const
{
    int m = static_cast<int>(T_m.rows());
    if (m < 2) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(T_m);
        Eigen::VectorXd ev = solver.eigenvalues();
        Eigen::MatrixXd evec = solver.eigenvectors();
        int take = std::min(K, m);
        Eigen::VectorXd good_evals(take);
        Eigen::MatrixXd good_evecs(m, take);
        for (int i = 0; i < take; ++i) {
            int idx = m - 1 - i;
            good_evals(i) = ev(idx);
            good_evecs.col(i) = evec.col(idx);
        }
        return CWFilterResult{good_evals, good_evecs};
    }

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_full(T_m);
    Eigen::VectorXd evals_full = solver_full.eigenvalues();
    Eigen::MatrixXd evecs_full = solver_full.eigenvectors();

    Eigen::MatrixXd T_s = T_m.bottomRightCorner(m - 1, m - 1);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_sub(T_s);
    Eigen::VectorXd evals_sub = solver_sub.eigenvalues();

    double tolerance = 1e-6 * std::max(1.0, T_m.norm());
    std::vector<int> good_indices;

    for (int i = 0; i < m; ++i) {
        double lambda = evals_full(i);
        bool is_ghost = false;
        for (int j = 0; j < m - 1; ++j) {
            if (std::abs(lambda - evals_sub(j)) < tolerance) {
                is_ghost = true;
                break;
            }
        }
        if (!is_ghost) {
            good_indices.push_back(i);
        }
    }

    std::sort(
        good_indices.begin(),
        good_indices.end(),
        [&](int a, int b) { return evals_full(a) > evals_full(b); });

    int n_good = std::min(K, static_cast<int>(good_indices.size()));
    Eigen::VectorXd good_evals(n_good);
    Eigen::MatrixXd good_evecs(m, n_good);

    for (int i = 0; i < n_good; ++i) {
        int idx = good_indices[i];
        good_evals(i) = evals_full(idx);
        good_evecs.col(i) = evecs_full.col(idx);
    }

    return CWFilterResult{good_evals, good_evecs};
}

Eigen::MatrixXd Client::trueTopEigenvectors(int K) const
{
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(C_);
    Eigen::MatrixXd vecs = solver.eigenvectors().rightCols(K);
    return vecs.rowwise().reverse();
}

double Client::eigenvalueMagnitudeGuess() const
{
    return C_.norm() / std::sqrt(static_cast<double>(d_));
}

double Client::lanczosResidualNormSqGuess() const
{
    // 对随机单位向量 v, E[||Cv - (v^T C v)v||²] = trace(C²)/d - (trace(C)/d)²
    // 归一化后 trace(C)=1, 所以 = trace(C²)/d - 1/d²
    double trace_C2 = (C_ * C_).trace();
    double d = static_cast<double>(d_);
    double est = trace_C2 / d - 1.0 / (d * d);
    return std::max(est, 1e-9);
}

double Client::normalizeCovariance()
{
    trace_C_ = C_.trace();
    if (trace_C_ < 1e-15) {
        throw std::runtime_error("normalizeCovariance: trace(C) is near zero");
    }
    C_ /= trace_C_;
    return trace_C_;
}

Eigen::MatrixXd Client::decryptLanczosVectors(
    const std::vector<seal::Ciphertext>& V_all) const
{
    const int m = static_cast<int>(V_all.size());
    Eigen::MatrixXd V_total(d_, m);

    for (int j = 0; j < m; ++j) {
        seal::Plaintext pt;
        decryptor_->decrypt(V_all[j], pt);
        std::vector<double> vals;
        encoder_->decode(pt, vals);
        for (int i = 0; i < d_; ++i) {
            V_total(i, j) = (i < static_cast<int>(vals.size())) ? vals[i] : 0.0;
        }
    }
    return V_total;
}

Eigen::MatrixXd Client::reconstructEigenvectors(
    const Eigen::MatrixXd& V_total_raw,
    const CWFilterResult& cw,
    int K) const
{
    const int m = static_cast<int>(V_total_raw.cols());
    const int n_good = static_cast<int>(cw.good_eigenvalues.size());
    const int take = std::min(K, n_good);

    if (take == 0 || m == 0) {
        return Eigen::MatrixXd::Zero(d_, std::max(1, K));
    }

    // Gram-Schmidt reorthogonalization on decrypted Lanczos basis
    Eigen::MatrixXd Q(d_, m);
    for (int j = 0; j < m; ++j) {
        Eigen::VectorXd v = V_total_raw.col(j);
        for (int k = 0; k < j; ++k) {
            v -= Q.col(k) * (Q.col(k).dot(v));
        }
        double nrm = v.norm();
        if (nrm > 1e-14) {
            Q.col(j) = v / nrm;
        } else {
            Q.col(j).setZero();
        }
    }

    // Ritz vectors s_i are the CW eigenvectors of T_m (size m x n_good)
    // Reconstruct approximate eigenvectors: u_i = Q * s_i
    const Eigen::MatrixXd& S = cw.good_eigenvectors; // m x n_good
    Eigen::MatrixXd U(d_, take);
    for (int i = 0; i < take; ++i) {
        Eigen::VectorXd s_i = S.col(i);
        if (s_i.size() > m) {
            s_i = s_i.head(m);
        }
        Eigen::VectorXd u = Q.leftCols(s_i.size()) * s_i;
        double nrm = u.norm();
        if (nrm > 1e-14) {
            U.col(i) = u / nrm;
        } else {
            U.col(i).setZero();
        }
    }
    return U;
}
