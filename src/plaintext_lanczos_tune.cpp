// 明文对照：与 Server 同构的 Lanczos 递推 + 同一套 CW，扫描 m，观察前 K 个 Ritz 相对真特征值的误差。
// 用于在加 HE 之前标定「m 取多大才够收敛」（例如相对误差 < 1%）。

#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>

#include <Eigen/Dense>

#include "client.h"

static Eigen::VectorXd randomUnitVector(int d, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> dist(0.0, 1.0);
    Eigen::VectorXd v(d);
    for (int i = 0; i < d; ++i) {
        v(i) = dist(rng);
    }
    v.normalize();
    return v;
}

int main()
{
    constexpr int d = 10;
    constexpr int p = 1;
    constexpr int m_max = 20;
    constexpr int K = 2;
    constexpr double target_rel_pct = 1.0;
    constexpr unsigned v_seed = 7;

    std::cout << "=== 明文 Lanczos 标定（同一 C、同一 v0、同一 CW） ===\n";
    std::cout << "d=" << d << ", K=" << K << ", 扫描 m=1.." << m_max
              << "，目标：前 " << K << " 个 Ritz（CW 后）相对真值误差均 ≤ " << target_rel_pct << "%\n\n";

    Client client(d, p, m_max);
    client.generateCovarianceMatrix();

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> true_solver(client.covMatrix());
    Eigen::VectorXd true_evals = true_solver.eigenvalues().reverse();

    double trace_C = client.normalizeCovariance();
    std::cout << "trace(C)=" << std::fixed << std::setprecision(2) << trace_C
              << "  归一化后特征值:";
    for (int i = 0; i < std::min(K + 1, d); ++i) {
        std::cout << " " << std::setprecision(4) << true_evals(i) / trace_C;
    }
    std::cout << "\n\n";

    Eigen::VectorXd v0 = randomUnitVector(d, v_seed);

    auto scan_and_report = [&](const char* title,
                               const std::function<Eigen::MatrixXd(const Client&, const Eigen::VectorXd&, int)>&
                                   make_t) {
        std::cout << title << "\n";
        std::cout << std::string(60, '-') << "\n";

        int best_m = -1;
        for (int m = 1; m <= m_max; ++m) {
            Eigen::MatrixXd T = make_t(client, v0, m);
            CWFilterResult cw = client.cullumWilloughbyFilter(T, K);

            const int n = static_cast<int>(cw.good_eigenvalues.size());
            double max_rel_pct = 0.0;
            std::string ritz_str;
            for (int i = 0; i < std::min(K, n); ++i) {
                const double tv = true_evals(i);
                const double rv = cw.good_eigenvalues(i) * trace_C;
                const double pct =
                    (std::abs(tv) > 1e-15) ? (std::abs(rv - tv) / std::abs(tv) * 100.0) : 0.0;
                max_rel_pct = std::max(max_rel_pct, pct);
                char buf[64];
                std::snprintf(buf, sizeof(buf), " λ%d=%.2f(%.1f%%)", i + 1, rv, pct);
                ritz_str += buf;
            }
            if (n < K) {
                max_rel_pct = 9999.0;
            }

            std::cout << "m=" << std::setw(2) << m << "  CW=" << n
                      << "  max_err=" << std::fixed << std::setprecision(1) << max_rel_pct << "%"
                      << ritz_str << "\n";

            if (best_m < 0 && n >= K && max_rel_pct <= target_rel_pct) {
                best_m = m;
            }
        }

        std::cout << std::string(60, '-') << "\n";
        if (best_m > 0) {
            std::cout << "满足 ≤" << target_rel_pct << "% 的最小 m: " << best_m << "\n\n";
        } else {
            std::cout << "在 m≤" << m_max << " 内未达到 ≤" << target_rel_pct << "%\n\n";
        }
    };

    scan_and_report(
        "[A] 标准对称 Lanczos（含 β_{j-1}v_{j-1}，用于论文/文档式验证）",
        [](const Client& c, const Eigen::VectorXd& v, int m) {
            return c.plaintextStandardLanczosTridiagonal(v, m);
        });

    scan_and_report(
        "[B] 与当前 Server 同构递推（无 v_{j-1} 项，与 he_pca Phase2 一致）",
        [](const Client& c, const Eigen::VectorXd& v, int m) {
            return c.plaintextMirrorHeLanczosTridiagonal(v, m);
        });

    std::cout << "说明: HE 还有 CKKS 噪声与 Newton 近似；[B] 与密态一致，[A] 表示「若实现完整 Lanczos」时的 m 需求。\n";
    return 0;
}
