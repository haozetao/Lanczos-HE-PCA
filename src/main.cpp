// FHE-PCA: 密态 Lanczos + Newton 归一化 + 特征向量重构
//
// 优化: 明文归一化 + 末步跳过 Newton + 明文预模拟初始猜测
// m_iter=2, Newton 2 次迭代, 总深度 18/19 层

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>

#include <Eigen/Dense>

#include "asor.h"
#include "client.h"
#include "server.h"

using Clock = std::chrono::high_resolution_clock;

static Eigen::VectorXd randomUnitVector(int d)
{
    std::mt19937 rng(7);
    std::normal_distribution<double> dist(0.0, 1.0);
    Eigen::VectorXd v(d);
    for (int i = 0; i < d; ++i) {
        v(i) = dist(rng);
    }
    v.normalize();
    return v;
}

static double cosineSimilarity(const Eigen::VectorXd& a, const Eigen::VectorXd& b)
{
    double na = a.norm();
    double nb = b.norm();
    if (na < 1e-15 || nb < 1e-15) return 0.0;
    return std::abs(a.dot(b)) / (na * nb);
}

int main()
{
    constexpr int d = 10;
    constexpr int p = 1;
    constexpr int m_iter = 2;
    constexpr int K = 3;
    constexpr int newton_iters = 2;

    std::cout << "=== FHE-PCA (d=" << d << ", m_iter=" << m_iter
              << ", Newton=" << newton_iters << ") ===" << std::endl;

    // ── Phase 1: Client 初始化 ──
    std::cout << "\n[Phase 1] Client setup" << std::endl;
    Client client(d, p, m_iter);
    client.generateCovarianceMatrix();

    Eigen::MatrixXd C_original = client.covMatrix();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> true_solver(C_original);
    Eigen::VectorXd true_evals = true_solver.eigenvalues().reverse();
    Eigen::MatrixXd true_evecs = true_solver.eigenvectors().rowwise().reverse();

    double trace_C = client.normalizeCovariance();
    std::cout << "  trace(C) = " << std::fixed << std::setprecision(4) << trace_C << std::endl;

    Eigen::VectorXd v0 = randomUnitVector(d);

    // 明文预模拟第一步 Lanczos: 获取精确的 ||W_new||² 作为 Newton 初始猜测
    const Eigen::MatrixXd& C_hat = client.covMatrix();
    Eigen::VectorXd w_sim = C_hat * v0;
    double alpha0_sim = v0.dot(w_sim);
    Eigen::VectorXd w_res = w_sim - alpha0_sim * v0;
    double norm_sq_sim = w_res.squaredNorm();
    double ev_guess = norm_sq_sim;

    std::cout << "  plaintext sim: α_0=" << std::setprecision(6) << alpha0_sim
              << "  ||W||²=" << std::scientific << std::setprecision(4) << norm_sq_sim
              << "  guess=1/√x=" << std::fixed << std::setprecision(2)
              << 1.0 / std::sqrt(norm_sq_sim) << std::endl;

    // ── Phase 2: Server 密态 Lanczos ──
    std::cout << "\n[Phase 2] Server HE-Lanczos" << std::endl;

    auto enc_C = client.encryptCovMatrix();
    auto enc_v = client.encryptColumnVector(v0);

    Server server(
        client.context(), client.publicKey(), client.relinKeys(),
        client.galoisKeys(), client.ckksScale());

    auto t0 = Clock::now();
    Server::LanczosResult lr = server.lanczosIteration(
        enc_C, enc_v, d, p, m_iter, ev_guess, newton_iters);
    auto t1 = Clock::now();
    double server_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "  耗时: " << std::fixed << std::setprecision(1)
              << server_ms << " ms" << std::endl;
    std::cout << "  α: " << lr.alphas.size() << "  β: " << lr.betas.size()
              << "  V_all: " << lr.V_all.size() << std::endl;

    // ── Phase 3: Client 后处理 ──
    std::cout << "\n[Phase 3] Client post-processing" << std::endl;

    int m = static_cast<int>(lr.alphas.size());
    Eigen::MatrixXd T = client.buildTridiagonalFromEncrypted(lr.alphas, lr.betas, m);
    std::cout << "  T_" << m << " =\n" << T << std::endl;

    CWFilterResult cw = client.cullumWilloughbyFilter(T, K);
    int n_good = static_cast<int>(cw.good_eigenvalues.size());
    std::cout << "  CW 保留特征值: " << n_good << std::endl;

    Eigen::MatrixXd V_total = client.decryptLanczosVectors(lr.V_all);
    Eigen::MatrixXd U = client.reconstructEigenvectors(V_total, cw, K);

    // ── Phase 4: 验证 ──
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  结果验证 (反归一化: ×" << std::setprecision(2) << trace_C << ")" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::cout << "\n  真实特征值:";
    for (int i = 0; i < std::min(K + 1, d); ++i) {
        std::cout << "  λ_" << (i + 1) << "=" << std::setprecision(2) << true_evals(i);
    }
    std::cout << std::endl;

    int show = std::min(K, n_good);
    for (int i = 0; i < show; ++i) {
        double he_val = cw.good_eigenvalues(i) * trace_C;
        double tv = true_evals(i);
        double err_pct = (std::abs(tv) > 1e-15)
            ? std::abs(he_val - tv) / std::abs(tv) * 100.0 : 0.0;

        std::cout << "\n  #" << (i + 1) << "  HE: " << std::setprecision(4) << he_val
                  << "  真实: " << tv
                  << "  误差: " << std::setprecision(2) << err_pct << "%";

        if (i < U.cols()) {
            double cs = cosineSimilarity(U.col(i), true_evecs.col(i));
            std::cout << "  cos_sim=" << std::setprecision(4) << cs;
        }
        std::cout << std::endl;
    }

    std::cout << "\n  Server 耗时: " << std::setprecision(1) << server_ms << " ms" << std::endl;
    std::cout << "\n=== 完成 ===" << std::endl;
    return 0;
}
