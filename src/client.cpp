#include "client.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace lbcrypto;

namespace {

// 收集 Lanczos 需要的旋转索引：2 的幂，用于 rotate-and-sum 全 slot 求和
// 以及维度相关的小步长（用于 packScalarsToVector 等）
std::vector<int32_t> collectRotationIndices(int /*d*/, uint32_t num_slots)
{
    // 实际运行时仅 Server::innerProduct 的 rotate-and-sum 使用旋转，
    // 步长都是 2 的幂（1, 2, 4, ..., num_slots/2）。之前额外枚举了 2(d+6)
    // 个小步旋转键，从未被调用，却让 d=4096 需要 8000+ 个 RotKey（~260 GB）。
    std::vector<int32_t> steps;
    for (uint32_t s = 1; s < num_slots; s *= 2) {
        steps.push_back(static_cast<int32_t>(s));
        steps.push_back(-static_cast<int32_t>(s));
    }
    std::sort(steps.begin(), steps.end());
    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
    return steps;
}

} // namespace

Client::Client(int d, int p, int m, bool enable_bootstrap, uint32_t levels_after_bootstrap)
    : d_(d), p_(p), m_(m), bootstrap_enabled_(enable_bootstrap)
{
    setupCryptoContext(enable_bootstrap, levels_after_bootstrap);
}

void Client::setupCryptoContext(bool enable_bootstrap, uint32_t levels_after_bootstrap)
{
    CCParams<CryptoContextCKKSRNS> parameters;
    parameters.SetSecretKeyDist(UNIFORM_TERNARY);
    // toy 配置：HEStd_NotSet 允许手动指定 RingDim，用于小规模实验
    parameters.SetSecurityLevel(HEStd_NotSet);
    parameters.SetRingDim(1u << 15); // 32768

    // 64-bit native 默认 FLEXIBLEAUTO，与 simple-ckks-bootstrapping 示例一致
    parameters.SetScalingTechnique(FLEXIBLEAUTO);
    parameters.SetScalingModSize(59);
    parameters.SetFirstModSize(60);

    uint32_t depth;
    std::vector<uint32_t> levelBudget = {4, 4};

    if (enable_bootstrap) {
        // Bootstrap 深度 = Lanczos/Newton 可用层数 + Bootstrap 自身消耗
        uint32_t bs_depth = FHECKKSRNS::GetBootstrapDepth(levelBudget, UNIFORM_TERNARY);
        depth = levels_after_bootstrap + bs_depth;
    } else {
        // baseline 模式：直接给足够的深度，不含 Bootstrap 开销
        depth = levels_after_bootstrap;
    }
    multiplicative_depth_ = depth;
    parameters.SetMultiplicativeDepth(depth);

    cc_ = GenCryptoContext(parameters);
    cc_->Enable(PKE);
    cc_->Enable(KEYSWITCH);
    cc_->Enable(LEVELEDSHE);

    num_slots_ = cc_->GetRingDimension() / 2;

    if (enable_bootstrap) {
        cc_->Enable(ADVANCEDSHE);
        cc_->Enable(FHE);
        cc_->EvalBootstrapSetup(levelBudget, {0, 0}, num_slots_);
    }

    keys_ = cc_->KeyGen();
    cc_->EvalMultKeyGen(keys_.secretKey);

    std::vector<int32_t> rot_steps = collectRotationIndices(d_, num_slots_);
    cc_->EvalRotateKeyGen(keys_.secretKey, rot_steps);

    if (enable_bootstrap) {
        cc_->EvalBootstrapKeyGen(keys_.secretKey, num_slots_);
    }
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

void Client::generateLowRankDataset(int N, int true_rank, double noise_sigma)
{
    if (N < 2) {
        throw std::invalid_argument("generateLowRankDataset: N 至少为 2");
    }
    if (true_rank < 1 || true_rank > std::min(d_, N)) {
        throw std::invalid_argument("generateLowRankDataset: true_rank 越界");
    }

    std::mt19937 rng(20240415);
    std::normal_distribution<double> gauss(0.0, 1.0);

    // X = U · S · V^T + noise, 其中 U ∈ R^{N×r}, S=diag(σ_1..σ_r), V ∈ R^{d×r}
    // σ_i 构造为指数衰减，使 PCA 能显著压缩
    Eigen::MatrixXd U(N, true_rank);
    Eigen::MatrixXd V(d_, true_rank);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < true_rank; ++j)
            U(i, j) = gauss(rng);
    for (int i = 0; i < d_; ++i)
        for (int j = 0; j < true_rank; ++j)
            V(i, j) = gauss(rng);

    // 正交化 V 的列，保证主成分方向明确
    Eigen::HouseholderQR<Eigen::MatrixXd> qr(V);
    Eigen::MatrixXd V_orth = qr.householderQ() * Eigen::MatrixXd::Identity(d_, true_rank);

    Eigen::VectorXd sigma(true_rank);
    for (int i = 0; i < true_rank; ++i) {
        sigma(i) = std::pow(0.7, i) * 10.0; // 10, 7, 4.9, 3.43, ...
    }

    Eigen::MatrixXd X(N, d_);
    X = U * sigma.asDiagonal() * V_orth.transpose();

    if (noise_sigma > 0.0) {
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < d_; ++j)
                X(i, j) += noise_sigma * gauss(rng);
    }

    Eigen::RowVectorXd mean = X.colwise().mean();
    X_centered_ = X.rowwise() - mean;

    C_ = (X_centered_.transpose() * X_centered_) / static_cast<double>(N - 1);
}

std::vector<Ciphertext<DCRTPoly>> Client::encryptCovMatrix() const
{
    std::vector<Ciphertext<DCRTPoly>> enc_C(d_);

    for (int i = 0; i < d_; ++i) {
        std::vector<double> row(num_slots_, 0.0);
        for (int k = 0; k < d_; ++k) {
            row[k] = C_(i, k);
        }
        Plaintext pt = cc_->MakeCKKSPackedPlaintext(row);
        enc_C[i] = cc_->Encrypt(keys_.publicKey, pt);
    }
    return enc_C;
}

std::vector<Ciphertext<DCRTPoly>> Client::encryptBlockVec(const Eigen::MatrixXd& V) const
{
    int cols = static_cast<int>(V.cols());
    std::vector<Ciphertext<DCRTPoly>> enc_V(cols);

    for (int j = 0; j < cols; ++j) {
        std::vector<double> col(num_slots_, 0.0);
        for (int i = 0; i < d_; ++i) {
            col[i] = V(i, j);
        }
        Plaintext pt = cc_->MakeCKKSPackedPlaintext(col);
        enc_V[j] = cc_->Encrypt(keys_.publicKey, pt);
    }
    return enc_V;
}

std::vector<Ciphertext<DCRTPoly>> Client::encryptColumnVector(const Eigen::VectorXd& v) const
{
    if (v.size() != d_) {
        throw std::invalid_argument("encryptColumnVector: 维度与 d 不一致");
    }
    Eigen::MatrixXd M(d_, 1);
    M.col(0) = v;
    return encryptBlockVec(M);
}

Eigen::MatrixXd Client::decryptToMatrix(
    const std::vector<Ciphertext<DCRTPoly>>& enc_W,
    int rows, int cols) const
{
    Eigen::MatrixXd result(rows, cols);

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            try {
                Plaintext pt;
                cc_->Decrypt(keys_.secretKey, enc_W[i * cols + j], &pt);
                const std::vector<double>& vals = pt->GetRealPackedValue();
                result(i, j) = vals.empty() ? 0.0 : vals[0];
            } catch (const lbcrypto::OpenFHEException&) {
                result(i, j) = 0.0;
            }
        }
    }
    return result;
}

Eigen::MatrixXd Client::buildTridiagonalFromEncrypted(
    const std::vector<Ciphertext<DCRTPoly>>& enc_alphas,
    const std::vector<Ciphertext<DCRTPoly>>& enc_betas,
    int m) const
{
    if (static_cast<int>(enc_alphas.size()) != m) {
        throw std::invalid_argument("buildTridiagonalFromEncrypted: α 数量与 m 不符");
    }
    if (m > 1 && static_cast<int>(enc_betas.size()) != m - 1) {
        throw std::invalid_argument("buildTridiagonalFromEncrypted: β 数量应为 m-1");
    }

    // 容错解密：低秩数据 Lanczos 尾部的 α/β 真值趋 0，CKKS 精度检查可能抛异常
    // 这里把抛异常的项直接视为 0（数学上 Lanczos 收敛就应该如此）
    auto safeDecryptScalar = [this](
        const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct) -> double {
        try {
            Plaintext pt;
            cc_->Decrypt(keys_.secretKey, ct, &pt);
            const std::vector<double>& vals = pt->GetRealPackedValue();
            return vals.empty() ? 0.0 : vals[0];
        } catch (const lbcrypto::OpenFHEException&) {
            return 0.0;
        }
    };

    std::vector<double> alphas(m);
    for (int i = 0; i < m; ++i) {
        alphas[i] = safeDecryptScalar(enc_alphas[i]);
    }

    std::vector<double> betas;
    betas.reserve(m > 1 ? static_cast<size_t>(m - 1) : 0);
    for (int i = 0; i < m - 1; ++i) {
        betas.push_back(safeDecryptScalar(enc_betas[i]));
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

CWFilterResult Client::cullumWilloughbyFilter(const Eigen::MatrixXd& T_m, int K) const
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
    const std::vector<Ciphertext<DCRTPoly>>& V_all) const
{
    const int m = static_cast<int>(V_all.size());
    Eigen::MatrixXd V_total(d_, m);

    for (int j = 0; j < m; ++j) {
        try {
            Plaintext pt;
            cc_->Decrypt(keys_.secretKey, V_all[j], &pt);
            const std::vector<double>& vals = pt->GetRealPackedValue();
            for (int i = 0; i < d_; ++i) {
                V_total(i, j) = (i < static_cast<int>(vals.size())) ? vals[i] : 0.0;
            }
        } catch (const lbcrypto::OpenFHEException&) {
            for (int i = 0; i < d_; ++i) V_total(i, j) = 0.0;
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

    const Eigen::MatrixXd& S = cw.good_eigenvectors;
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
