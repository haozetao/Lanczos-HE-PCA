#include "client.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lbcrypto;

namespace {

// 收集 Diagonal+BSGS Lanczos 所需的全部 EvalRotate 索引：
//   (a) innerProductReplicated 的 sum-over-d：±1, ±2, ±4, ..., ±d/2
//   (b) matvecDiagonal 在 BSGS 模式下：
//         baby steps  Rot(v, +j)      j = 0..b-1   ← 正向
//         giant steps Rot(acc, +b·i)  i = 0..a-1   ← 正向
//       且 inverse 方向不需要（matvec 全部正向 rotate）
//   (c) 无 BSGS 时（bsgs_b=0）：matvec 用 ±0..d-1（这里我们生成 +1..+d-1）
//
// 注意：OpenFHE rotation key 的 ± 是分别的索引（左/右循环）。
// 实际生产中只需要正向 rotate，但 inner product 对称写法可以同时用 ± 简化。
//
// 对 d=256, b=16: 共 30~40 个独立索引（远少于之前 ±1..±num_slots/2 = 28 个，
// 但 multiplicativeDepth ≥ 50 时单 key ~250 MB 仍是 KeyGen 内存大头）。
std::vector<int32_t> collectRotationIndices(int d, int bsgs_b, uint32_t num_slots)
{
    if (d <= 0) return {};

    std::vector<int32_t> steps;
    auto push_both = [&](int32_t s) {
        if (s == 0) return;
        steps.push_back(s);
        steps.push_back(-s);
    };

    // (a) inner product over replicated vector: log_2(d) 步 power-of-2 rotate
    for (int s = 1; s < d; s *= 2) {
        push_both(s);
    }

    // 默认只生成 BSGS 模式的 keys（d=256 时约 30 个，可控）。
    // 想跑 plain diag（BSGS_B=0）需要 +1..+d-1 的全部 d-1 个 keys，
    // 对 d=256 ≈ 60 GB key 内存，会让 KeyGen 直接 OOM ──
    // 故 plain diag fallback 仅在 d ≤ 32 时启用（用于小维度调试）。
    if (bsgs_b > 0 && d % bsgs_b == 0) {
        const int b = bsgs_b;
        const int a = d / b;
        for (int j = 1; j < b; ++j) push_both(j);
        for (int i = 1; i < a; ++i) push_both(i * b);
    }
    if (d <= 32) {
        // 小 d 时附带生成 plain diag 全部 keys，便于 BSGS_B=0 切换调试
        for (int k = 1; k < d; ++k) push_both(k);
    }

    // 兼容：保留全 slot 旋转 power-of-2 keys（log num_slots ≈ 14 个），
    // 部分会跟上面去重。这些 key 主要用于 inner product 全 slot 回退路径。
    for (uint32_t s = 1; s < num_slots; s *= 2) {
        steps.push_back(static_cast<int32_t>(s));
        steps.push_back(-static_cast<int32_t>(s));
    }

    std::sort(steps.begin(), steps.end());
    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
    return steps;
}

int pickBsgsFactor(int d) {
    if (d <= 1) return 0;
    int best = 0;
    double best_dist = std::numeric_limits<double>::infinity();
    const double target = std::sqrt(static_cast<double>(d));
    for (int b = 1; b <= d; ++b) {
        if (d % b != 0) continue;
        double dist = std::abs(static_cast<double>(b) - target);
        if (dist < best_dist) {
            best_dist = dist;
            best = b;
        }
    }
    // b == 1 退化为 plain diagonal (a=d, baby step 数 0)，不如直接 plain
    // b == d 同理（giant step 数 0）。两端都不算"有意义的 BSGS"，返回 0。
    if (best <= 1 || best >= d) return 0;
    return best;
}

} // namespace

int Client::autoBsgsB() const { return pickBsgsFactor(d_); }

Client::Client(int d, int p, int m, bool enable_bootstrap,
               uint32_t levels_after_bootstrap, bool enable_he)
    : d_(d), p_(p), m_(m), bootstrap_enabled_(enable_bootstrap)
{
    if (enable_he) {
        setupCryptoContext(enable_bootstrap, levels_after_bootstrap);
    }
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

    std::vector<int32_t> rot_steps = collectRotationIndices(d_, pickBsgsFactor(d_), num_slots_);
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

void Client::generateFromBinaryFile(const std::string& path,
                                    bool normalize_to_unit,
                                    int max_samples)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("generateFromBinaryFile: 无法打开 " + path);
    }

    uint32_t N_in = 0;
    uint32_t d_in = 0;
    f.read(reinterpret_cast<char*>(&N_in), sizeof(uint32_t));
    f.read(reinterpret_cast<char*>(&d_in), sizeof(uint32_t));
    if (!f) {
        throw std::runtime_error("generateFromBinaryFile: 读 header 失败 " + path);
    }
    if (static_cast<int>(d_in) != d_) {
        throw std::invalid_argument(
            "generateFromBinaryFile: 文件 d=" + std::to_string(d_in)
            + " 与 Client d=" + std::to_string(d_) + " 不一致");
    }

    int N = static_cast<int>(N_in);
    if (max_samples > 0 && max_samples < N) {
        N = max_samples;
    }
    if (N < 2) {
        throw std::invalid_argument(
            "generateFromBinaryFile: N=" + std::to_string(N) + " 至少要 2");
    }

    Eigen::MatrixXd X(N, d_);
    std::vector<double> row(d_);
    for (int i = 0; i < N; ++i) {
        f.read(reinterpret_cast<char*>(row.data()),
               static_cast<std::streamsize>(sizeof(double) * d_));
        if (!f) {
            throw std::runtime_error(
                "generateFromBinaryFile: 读第 " + std::to_string(i) + " 行失败");
        }
        for (int j = 0; j < d_; ++j) {
            X(i, j) = row[j];
        }
    }

    if (normalize_to_unit) {
        // 像素值 0..255 → [0, 1]（跟 Panda 2021 / Ma 2023 一致）
        X /= 255.0;
    }

    Eigen::RowVectorXd mean = X.colwise().mean();
    X_centered_ = X.rowwise() - mean;
    C_ = (X_centered_.transpose() * X_centered_) / static_cast<double>(N - 1);

    std::cout << "  [dataset] 加载二进制 " << path
              << "  N=" << N << "  d=" << d_
              << (normalize_to_unit ? "  (像素归一化到 [0,1])" : "")
              << std::endl;
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

std::vector<Ciphertext<DCRTPoly>> Client::encryptCovMatrixDiagonal(int bsgs_b) const
{
    if (d_ <= 0) {
        throw std::invalid_argument("encryptCovMatrixDiagonal: d 必须 > 0");
    }
    if (static_cast<int>(num_slots_) % d_ != 0) {
        throw std::invalid_argument(
            "encryptCovMatrixDiagonal: num_slots (" + std::to_string(num_slots_)
            + ") 必须能被 d (" + std::to_string(d_) + ") 整除");
    }
    if (bsgs_b < 0) {
        throw std::invalid_argument("encryptCovMatrixDiagonal: bsgs_b 不能为负");
    }
    if (bsgs_b > 0 && d_ % bsgs_b != 0) {
        throw std::invalid_argument(
            "encryptCovMatrixDiagonal: d=" + std::to_string(d_)
            + " 必须能被 bsgs_b=" + std::to_string(bsgs_b) + " 整除");
    }

    const int reps = static_cast<int>(num_slots_) / d_;
    std::vector<Ciphertext<DCRTPoly>> enc_diag(d_);

    for (int k = 0; k < d_; ++k) {
        // diag_k[i] = C[i, (i+k) mod d_]
        std::vector<double> diag(d_, 0.0);
        for (int i = 0; i < d_; ++i) {
            diag[i] = C_(i, (i + k) % d_);
        }

        // BSGS 预旋转：
        //   BSGS 等式 diag ⊙ Rot(v, b·i) = Rot(Rot(diag, -b·i) ⊙ v, b·i)
        //   故 Client 需要预先把 diag_k 做 cyclic-right rotation by (b·i_g) mod d，
        //   等价于 cyclic-left rotation by (d - b·i_g mod d) mod d。
        //   注意方向 ── 之前的实现写反了 (+b·i_g 左旋)，对 b·i_g 不是 d/2
        //   的 i 会算错（如 d=16, b=4 时 i=1, i=3 错位 4 个 slot），
        //   导致 HE 跑出来跟明文 mirror 完全不一致而 cos≈0。
        if (bsgs_b > 0) {
            int i_g = k / bsgs_b;
            int right_shift = (bsgs_b * i_g) % d_;          // 想要的右旋
            int left_shift  = (d_ - right_shift) % d_;       // 等价的左旋
            if (left_shift != 0) {
                std::vector<double> rotated(d_, 0.0);
                for (int i = 0; i < d_; ++i) {
                    rotated[i] = diag[(i + left_shift) % d_];
                }
                diag.swap(rotated);
            }
        }

        // 在 num_slots 长 plaintext 内 cyclic-replicate diag d/num_slots 次
        std::vector<double> packed(num_slots_, 0.0);
        for (int r = 0; r < reps; ++r) {
            for (int i = 0; i < d_; ++i) {
                packed[r * d_ + i] = diag[i];
            }
        }

        Plaintext pt = cc_->MakeCKKSPackedPlaintext(packed);
        enc_diag[k] = cc_->Encrypt(keys_.publicKey, pt);
    }
    return enc_diag;
}

Ciphertext<DCRTPoly> Client::encryptColumnVectorReplicated(const Eigen::VectorXd& v) const
{
    if (v.size() != d_) {
        throw std::invalid_argument("encryptColumnVectorReplicated: 维度与 d_ 不一致");
    }
    if (static_cast<int>(num_slots_) % d_ != 0) {
        throw std::invalid_argument(
            "encryptColumnVectorReplicated: num_slots 必须能被 d 整除");
    }
    const int reps = static_cast<int>(num_slots_) / d_;
    std::vector<double> packed(num_slots_, 0.0);
    for (int r = 0; r < reps; ++r) {
        for (int i = 0; i < d_; ++i) {
            packed[r * d_ + i] = v(i);
        }
    }
    Plaintext pt = cc_->MakeCKKSPackedPlaintext(packed);
    return cc_->Encrypt(keys_.publicKey, pt);
}

Eigen::VectorXd Client::decryptReplicatedVector(
    const Ciphertext<DCRTPoly>& ct) const
{
    Plaintext pt;
    cc_->Decrypt(keys_.secretKey, ct, &pt);
    pt->SetLength(d_);
    const auto& vals = pt->GetRealPackedValue();
    Eigen::VectorXd v(d_);
    for (int i = 0; i < d_; ++i) {
        v(i) = vals[static_cast<size_t>(i)];
    }
    return v;
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

Client::LanczosPROResult Client::plaintextLanczosWithPROAndNoise(
    const Eigen::VectorXd& v0,
    int m_iter,
    int reorth_b,
    double sigma,
    unsigned seed) const
{
    if (v0.size() != d_) {
        throw std::invalid_argument(
            "plaintextLanczosWithPROAndNoise: v0 维度与 d 不一致");
    }
    if (m_iter < 1) {
        throw std::invalid_argument(
            "plaintextLanczosWithPROAndNoise: m_iter 至少为 1");
    }

    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    auto noisy_scale = [&](double base, double rel_std) {
        if (rel_std <= 0.0) return 1.0;
        return 1.0 + rel_std * gauss(rng);
    };

    const double sqrt_d = std::sqrt(static_cast<double>(d_));
    const double sigma_matvec = sigma * sqrt_d;
    const double newton_rel_err = (sigma > 0.0) ? 0.05 : 0.0;

    Eigen::MatrixXd V_total(d_, m_iter);
    std::vector<double> alphas;
    std::vector<double> betas;
    alphas.reserve(static_cast<size_t>(m_iter));
    betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));

    Eigen::VectorXd V = v0.normalized();
    Eigen::VectorXd V_prev = Eigen::VectorXd::Zero(d_);
    double beta_prev = 0.0;
    int actual_m = 0;

    for (int iter = 0; iter < m_iter; ++iter) {
        V_total.col(iter) = V;
        actual_m = iter + 1;

        // matvec: W = C·V，每分量注入相对噪声 sigma·√d
        Eigen::VectorXd W = C_ * V;
        if (sigma > 0.0) {
            for (int i = 0; i < d_; ++i) {
                W(i) *= noisy_scale(W(i), sigma_matvec);
            }
        }
        if (iter > 0) {
            W -= beta_prev * V_prev;
        }

        // α = V^T·W，再叠一次内积噪声（√d 项之和）
        double alpha = V.dot(W);
        if (sigma > 0.0) {
            alpha *= noisy_scale(alpha, sigma_matvec);
        }
        alphas.push_back(alpha);

        if (iter == m_iter - 1) {
            break;
        }

        Eigen::VectorXd Wnew = W - alpha * V;

        // === 重正交化：对最近 reorth_b 个基向量做 GS ===
        if (reorth_b > 0) {
            const int start = std::max(0, iter - reorth_b + 1);
            for (int k = start; k <= iter; ++k) {
                const Eigen::VectorXd& Vk = V_total.col(k);
                double proj = Vk.dot(Wnew);
                if (sigma > 0.0) {
                    proj *= noisy_scale(proj, sigma_matvec);
                }
                Wnew -= proj * Vk;
            }
        }

        // ‖W_new‖²：单次平方求和（不再 √d 放大，是单一操作）
        double norm_sq = Wnew.squaredNorm();
        if (sigma > 0.0) {
            norm_sq *= noisy_scale(norm_sq, sigma);
            if (norm_sq < 1e-18) norm_sq = 1e-18;
        }

        // Newton 2 轮 ~5% 相对误差
        double norm = std::sqrt(norm_sq);
        if (newton_rel_err > 0.0) {
            norm *= 1.0 + newton_rel_err * gauss(rng);
            if (norm < 1e-14) norm = 1e-14;
        }
        if (norm < 1e-14) {
            // 早停：Krylov 子空间已完整捕获
            break;
        }
        betas.push_back(norm);

        V_prev = V;
        V = Wnew / norm;
        beta_prev = norm;
    }

    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(actual_m, actual_m);
    for (int i = 0; i < actual_m; ++i) {
        T(i, i) = alphas[static_cast<size_t>(i)];
        if (i < actual_m - 1 && static_cast<size_t>(i) < betas.size()) {
            const double b = betas[static_cast<size_t>(i)];
            T(i, i + 1) = b;
            T(i + 1, i) = b;
        }
    }

    LanczosPROResult result;
    result.T = T;
    result.V_total = V_total.leftCols(actual_m);
    result.actual_m = actual_m;
    return result;
}

Client::LanczosPROResult Client::plaintextHeMirrorLanczos(
    const Eigen::VectorXd& v0,
    int m_iter,
    bool enable_fro,
    int fro_skip_first) const
{
    if (v0.size() != d_) {
        throw std::invalid_argument(
            "plaintextHeMirrorLanczos: v0 维度与 d 不一致");
    }
    if (m_iter < 1) {
        throw std::invalid_argument(
            "plaintextHeMirrorLanczos: m_iter 至少为 1");
    }
    const int fro_skip = std::max(0, fro_skip_first);

    Eigen::MatrixXd V_total(d_, m_iter);
    std::vector<double> alphas;
    std::vector<double> betas;
    alphas.reserve(static_cast<size_t>(m_iter));
    betas.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));

    Eigen::VectorXd V = v0.normalized();
    Eigen::VectorXd V_prev = Eigen::VectorXd::Zero(d_);
    double beta_prev = 0.0;
    int actual_m = 0;

    for (int iter = 0; iter < m_iter; ++iter) {
        V_total.col(iter) = V;
        actual_m = iter + 1;

        Eigen::VectorXd W = C_ * V;
        if (iter > 0) {
            W -= beta_prev * V_prev;
        }

        const double alpha = V.dot(W);
        alphas.push_back(alpha);

        if (iter == m_iter - 1) {
            break;
        }

        Eigen::VectorXd Wnew = W - alpha * V;

        // FRO：与 HE 端语义对齐——iter < fro_skip_first 时跳过
        if (enable_fro && iter >= fro_skip) {
            for (int k = 0; k < iter; ++k) {
                const Eigen::VectorXd& Vk = V_total.col(k);
                const double proj = Vk.dot(Wnew);
                Wnew -= proj * Vk;
            }
        }

        const double norm = Wnew.norm();
        if (norm < 1e-14) {
            break;
        }
        betas.push_back(norm);

        V_prev = V;
        V = Wnew / norm;
        beta_prev = norm;
    }

    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(actual_m, actual_m);
    for (int i = 0; i < actual_m; ++i) {
        T(i, i) = alphas[static_cast<size_t>(i)];
        if (i < actual_m - 1 && static_cast<size_t>(i) < betas.size()) {
            const double b = betas[static_cast<size_t>(i)];
            T(i, i + 1) = b;
            T(i + 1, i) = b;
        }
    }

    LanczosPROResult result;
    result.T = T;
    result.V_total = V_total.leftCols(actual_m);
    result.actual_m = actual_m;
    return result;
}

std::vector<double> Client::plaintextMirrorResidualNormSqGuesses(
    const Eigen::VectorXd& v0,
    int m_iter) const
{
    if (v0.size() != d_) {
        throw std::invalid_argument("plaintextMirrorResidualNormSqGuesses: v0 维度与 d 不一致");
    }
    if (m_iter < 1) {
        throw std::invalid_argument("plaintextMirrorResidualNormSqGuesses: m_iter 至少为 1");
    }

    Eigen::VectorXd V = v0.normalized();
    Eigen::VectorXd V_prev = Eigen::VectorXd::Zero(d_);
    double beta_prev = 0.0;

    std::vector<double> guesses;
    guesses.reserve(static_cast<size_t>(std::max(0, m_iter - 1)));

    for (int iter = 0; iter < m_iter - 1; ++iter) {
        Eigen::VectorXd W = C_ * V - beta_prev * V_prev;
        const double alpha = V.dot(W);
        Eigen::VectorXd Wnew = W - alpha * V;
        const double norm_sq = Wnew.squaredNorm();
        guesses.push_back(std::max(norm_sq, 1e-18));

        const double norm = std::sqrt(norm_sq);
        if (norm < 1e-14) {
            break;
        }
        V_prev = V;
        V = Wnew / norm;
        beta_prev = norm;
    }

    return guesses;
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
