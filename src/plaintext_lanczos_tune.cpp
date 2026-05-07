// 明文 Lanczos + 噪声注入 + 部分重正交化 (PRO) 验证
// 用于在不动 HE 代码的前提下，验证 PRO 是否真的能救回 K≥3 的精度。
//
// 噪声模型见 Client::plaintextLanczosWithPROAndNoise。
//
// 用法（环境变量驱动）：
//   D=256 K=4 M_ITER=5 RANK=30 NOISE=2.0 SIGMA=1e-3 ./plaintext_lanczos_tune
//
// 默认会扫描 (d, sigma, reorth_b) 多组配置，输出 λ 误差和 cos_sim。

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "client.h"

// ============================================================================
// Selective FRO 实验：在指定的步集合 fro_steps 上做完整重正交化，其余步不做。
// 模拟 HE 中"挑触发节点降低 BS 次数"的策略。
//
// 噪声模型与 Client::plaintextLanczosWithPROAndNoise 一致。
// 直接在工具内实现，不污染 client.h/cpp。
// ============================================================================
struct SelectiveLanczosResult {
    Eigen::MatrixXd T;
    Eigen::MatrixXd V_total;
    int actual_m;
};

static SelectiveLanczosResult runSelectiveFROLanczos(
    const Eigen::MatrixXd& C_normalized,
    int d,
    const Eigen::VectorXd& v0,
    int m_iter,
    const std::set<int>& fro_steps,
    double sigma,
    unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    auto noisy = [&](double base, double rel_std) {
        return (rel_std <= 0.0) ? 1.0 : 1.0 + rel_std * gauss(rng);
    };

    const double sqrt_d = std::sqrt(static_cast<double>(d));
    const double sigma_matvec = sigma * sqrt_d;
    const double newton_rel_err = (sigma > 0.0) ? 0.05 : 0.0;

    Eigen::MatrixXd V_total(d, m_iter);
    std::vector<double> alphas;
    std::vector<double> betas;

    Eigen::VectorXd V = v0.normalized();
    Eigen::VectorXd V_prev = Eigen::VectorXd::Zero(d);
    double beta_prev = 0.0;
    int actual_m = 0;

    for (int iter = 0; iter < m_iter; ++iter) {
        V_total.col(iter) = V;
        actual_m = iter + 1;

        Eigen::VectorXd W = C_normalized * V;
        if (sigma > 0.0) {
            for (int i = 0; i < d; ++i) W(i) *= noisy(W(i), sigma_matvec);
        }
        if (iter > 0) W -= beta_prev * V_prev;

        double alpha = V.dot(W);
        if (sigma > 0.0) alpha *= noisy(alpha, sigma_matvec);
        alphas.push_back(alpha);

        if (iter == m_iter - 1) break;

        Eigen::VectorXd Wnew = W - alpha * V;

        // === Selective FRO：仅在 iter ∈ fro_steps 时做完整重正交化 ===
        if (fro_steps.count(iter) > 0) {
            for (int k = 0; k <= iter; ++k) {
                const Eigen::VectorXd& Vk = V_total.col(k);
                double proj = Vk.dot(Wnew);
                if (sigma > 0.0) proj *= noisy(proj, sigma_matvec);
                Wnew -= proj * Vk;
            }
        }

        double norm_sq = Wnew.squaredNorm();
        if (sigma > 0.0) {
            norm_sq *= noisy(norm_sq, sigma);
            if (norm_sq < 1e-18) norm_sq = 1e-18;
        }

        double norm = std::sqrt(norm_sq);
        if (newton_rel_err > 0.0) {
            norm *= 1.0 + newton_rel_err * gauss(rng);
            if (norm < 1e-14) norm = 1e-14;
        }
        if (norm < 1e-14) break;
        betas.push_back(norm);

        V_prev = V;
        V = Wnew / norm;
        beta_prev = norm;
    }

    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(actual_m, actual_m);
    for (int i = 0; i < actual_m; ++i) {
        T(i, i) = alphas[i];
        if (i < actual_m - 1 && static_cast<size_t>(i) < betas.size()) {
            T(i, i + 1) = T(i + 1, i) = betas[i];
        }
    }

    return SelectiveLanczosResult{T, V_total.leftCols(actual_m), actual_m};
}

static int envInt(const char* name, int dflt)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::atoi(v) : dflt;
}

static double envDouble(const char* name, double dflt)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::atof(v) : dflt;
}

static const char* envStr(const char* name, const char* dflt)
{
    const char* v = std::getenv(name);
    return (v && *v) ? v : dflt;
}

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

static double cosineSimilarity(const Eigen::VectorXd& a, const Eigen::VectorXd& b)
{
    double na = a.norm();
    double nb = b.norm();
    if (na < 1e-15 || nb < 1e-15) return 0.0;
    return std::abs(a.dot(b)) / (na * nb);
}

// 从 T_m + V_total 重构特征向量（与 Client::reconstructEigenvectors 类似但
// 在明文上下文，更简洁）：
//   1. T_m 特征分解 -> Ritz 值 + Ritz 向量 s_i
//   2. 对 V_total 做 Gram-Schmidt 修正（HE 噪声会破坏正交性，模拟里也做）
//   3. u_i = Q · s_i；归一化
struct ReconstructResult {
    Eigen::VectorXd ritz_values;       // 已按降序排好（取前 K）
    Eigen::MatrixXd eigenvectors;      // d × K
};

static ReconstructResult reconstructFromTV(
    const Eigen::MatrixXd& T,
    const Eigen::MatrixXd& V_total,
    int K)
{
    const int m = static_cast<int>(T.rows());
    const int d = static_cast<int>(V_total.rows());

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(T);
    Eigen::VectorXd evals = solver.eigenvalues();
    Eigen::MatrixXd evecs = solver.eigenvectors();

    // 排序：降序
    std::vector<int> order(m);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return evals(a) > evals(b); });

    int take = std::min(K, m);
    Eigen::VectorXd ritz(take);
    Eigen::MatrixXd S(m, take);
    for (int i = 0; i < take; ++i) {
        ritz(i) = evals(order[i]);
        S.col(i) = evecs.col(order[i]);
    }

    // 对 V_total 做 GS（与 client.reconstructEigenvectors 同步）
    Eigen::MatrixXd Q(d, m);
    for (int j = 0; j < m; ++j) {
        Eigen::VectorXd v = V_total.col(j);
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

    Eigen::MatrixXd U(d, take);
    for (int i = 0; i < take; ++i) {
        Eigen::VectorXd u = Q * S.col(i);
        double nrm = u.norm();
        U.col(i) = (nrm > 1e-14) ? u / nrm : u;
    }

    return ReconstructResult{ritz, U};
}

struct RunRecord {
    double sigma;
    int    reorth_b;
    double lambda1_err_pct;
    double lambda2_err_pct;
    double lambda3_err_pct;
    double lambda4_err_pct;
    double cos1, cos2, cos3, cos4;
    double r2v;
};

// 执行一次 (sigma, reorth_b) 配置，对 N_TRIALS 次随机扰动取平均
static RunRecord runOne(
    const Client& client,
    const Eigen::VectorXd& v0,
    int m_iter,
    int K,
    double sigma,
    int reorth_b,
    int n_trials,
    const Eigen::VectorXd& true_evals_unnorm,
    const Eigen::MatrixXd& true_evecs,
    double trace_C)
{
    std::vector<double> err_acc(K, 0.0);
    std::vector<double> cos_acc(K, 0.0);
    double r2v_acc = 0.0;

    for (int trial = 0; trial < n_trials; ++trial) {
        unsigned seed = 1000u + static_cast<unsigned>(trial);
        Client::LanczosPROResult lr = client.plaintextLanczosWithPROAndNoise(
            v0, m_iter, reorth_b, sigma, seed);

        ReconstructResult rec = reconstructFromTV(lr.T, lr.V_total, K);

        // λ 误差（反归一化）
        for (int i = 0; i < K; ++i) {
            double he = (i < rec.ritz_values.size())
                          ? rec.ritz_values(i) * trace_C : 0.0;
            double tv = (i < true_evals_unnorm.size()) ? true_evals_unnorm(i) : 0.0;
            double pct = (std::abs(tv) > 1e-15)
                ? std::abs(he - tv) / std::abs(tv) * 100.0 : 0.0;
            err_acc[i] += pct;

            double cs = (i < rec.eigenvectors.cols() && i < true_evecs.cols())
                ? cosineSimilarity(rec.eigenvectors.col(i), true_evecs.col(i))
                : 0.0;
            cos_acc[i] += cs;
        }

        // R²(V)：对前 K 列计算
        if (rec.eigenvectors.cols() == K && true_evecs.cols() >= K) {
            Eigen::MatrixXd V_aligned(rec.eigenvectors.rows(), K);
            for (int i = 0; i < K; ++i) {
                double sgn = (true_evecs.col(i).dot(rec.eigenvectors.col(i)) >= 0)
                                 ? 1.0 : -1.0;
                V_aligned.col(i) = sgn * rec.eigenvectors.col(i);
            }
            double num = (V_aligned - true_evecs.leftCols(K)).squaredNorm();
            double den = true_evecs.leftCols(K).squaredNorm();
            r2v_acc += (den > 1e-15) ? (1.0 - num / den) : 0.0;
        }
    }

    RunRecord rec{};
    rec.sigma = sigma;
    rec.reorth_b = reorth_b;
    auto avg = [&](int i, const std::vector<double>& v) {
        return (i < static_cast<int>(v.size())) ? v[i] / n_trials : 0.0;
    };
    rec.lambda1_err_pct = avg(0, err_acc);
    rec.lambda2_err_pct = avg(1, err_acc);
    rec.lambda3_err_pct = avg(2, err_acc);
    rec.lambda4_err_pct = avg(3, err_acc);
    rec.cos1 = avg(0, cos_acc);
    rec.cos2 = avg(1, cos_acc);
    rec.cos3 = avg(2, cos_acc);
    rec.cos4 = avg(3, cos_acc);
    rec.r2v = r2v_acc / n_trials;
    return rec;
}

static void printHeader()
{
    std::cout << "sigma     b     "
              << "λ₁_err%  λ₂_err%  λ₃_err%  λ₄_err%  "
              << "cos₁    cos₂    cos₃    cos₄    R²(V)\n";
    std::cout << std::string(110, '-') << "\n";
}

static void printRow(const RunRecord& r)
{
    auto fmtErr = [](double v) {
        char buf[16];
        if (v >= 9999) std::snprintf(buf, sizeof(buf), "  ----- ");
        else std::snprintf(buf, sizeof(buf), "%7.2f%% ", v);
        return std::string(buf);
    };
    auto fmtCos = [](double v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%6.3f  ", v);
        return std::string(buf);
    };

    std::cout << std::scientific << std::setprecision(1) << r.sigma
              << "  " << std::setw(3) << r.reorth_b << "   "
              << fmtErr(r.lambda1_err_pct)
              << fmtErr(r.lambda2_err_pct)
              << fmtErr(r.lambda3_err_pct)
              << fmtErr(r.lambda4_err_pct)
              << fmtCos(r.cos1)
              << fmtCos(r.cos2)
              << fmtCos(r.cos3)
              << fmtCos(r.cos4)
              << std::fixed << std::setprecision(4) << r.r2v
              << "\n";
}

int main()
{
    const int d = envInt("D", 256);
    const int K = envInt("K", 4);
    const int m_iter = envInt("M_ITER", 5);
    const int rank = envInt("RANK", std::min(d, 30));
    const double noise = envDouble("NOISE", 2.0);
    const int dataset_N = envInt("DATASET_N", 500);
    const int n_trials = envInt("N_TRIALS", 5);
    const unsigned v_seed = static_cast<unsigned>(envInt("V_SEED", 7));

    // sigma 列表：默认覆盖估算的 CKKS 单步相对噪声范围
    // d=50 时 √d ≈ 7,    实际 σ ≈ 1e-4
    // d=256 时 √d ≈ 16,  实际 σ ≈ 1e-3
    // d=784 时 √d ≈ 28,  实际 σ ≈ 1e-3 ~ 1e-2
    std::vector<double> sigma_list;
    const char* sigma_env = envStr("SIGMA_LIST", "");
    if (sigma_env && *sigma_env) {
        std::string s(sigma_env);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) comma = s.size();
            sigma_list.push_back(std::atof(s.substr(pos, comma - pos).c_str()));
            pos = comma + 1;
        }
    } else {
        sigma_list = {0.0, 1e-4, 1e-3, 5e-3, 1e-2};
    }

    // reorth_b 列表
    std::vector<int> b_list;
    const char* b_env = envStr("B_LIST", "");
    if (b_env && *b_env) {
        std::string s(b_env);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) comma = s.size();
            b_list.push_back(std::atoi(s.substr(pos, comma - pos).c_str()));
            pos = comma + 1;
        }
    } else {
        b_list = {0, 2, m_iter};  // 不重正交 / Local PRO b=2 / 完整 FRO
    }

    std::cout << "=== 明文 Lanczos + 噪声注入 + PRO 验证 ===\n";
    std::cout << "d=" << d << "  K=" << K << "  m_iter=" << m_iter
              << "  rank=" << rank << "  noise=" << noise
              << "  trials=" << n_trials << "\n\n";

    Client client(d, /*p*/ 1, m_iter);
    if (dataset_N > 0) {
        client.generateLowRankDataset(dataset_N, rank, noise);
    } else {
        client.generateCovarianceMatrix();
    }

    Eigen::MatrixXd C_orig = client.covMatrix();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> true_solver(C_orig);
    Eigen::VectorXd true_evals = true_solver.eigenvalues().reverse();
    Eigen::MatrixXd true_evecs_full = true_solver.eigenvectors().rowwise().reverse();
    Eigen::MatrixXd true_evecs_K = true_evecs_full.leftCols(std::min(K, d));

    double trace_C = client.normalizeCovariance();
    std::cout << "trace(C)=" << std::fixed << std::setprecision(2) << trace_C
              << "  真实 λ:";
    for (int i = 0; i < std::min(K + 1, d); ++i) {
        std::cout << "  λ" << (i + 1) << "=" << std::setprecision(2) << true_evals(i);
    }
    std::cout << "\n\n";

    Eigen::VectorXd v0 = randomUnitVector(d, v_seed);

    const std::string mode = envStr("MODE", "pro");

    if (mode == "selective") {
        // ── Selective FRO 扫描 ──
        // SCHEDULES 环境变量：分号分隔的多组步集合，每组用逗号分隔
        // 例如 SCHEDULES="3;4;3,7;2,5,7;1,2,3,4,5,6,7"
        std::vector<std::set<int>> schedules;
        std::vector<std::string> labels;

        const char* sched_env = envStr("SCHEDULES", "");
        if (sched_env && *sched_env) {
            std::string s(sched_env);
            size_t pos = 0;
            while (pos < s.size()) {
                size_t semi = s.find(';', pos);
                if (semi == std::string::npos) semi = s.size();
                std::string group = s.substr(pos, semi - pos);
                std::set<int> ss;
                size_t pp = 0;
                while (pp < group.size()) {
                    size_t cc = group.find(',', pp);
                    if (cc == std::string::npos) cc = group.size();
                    if (cc > pp) ss.insert(std::atoi(group.substr(pp, cc - pp).c_str()));
                    pp = cc + 1;
                }
                schedules.push_back(ss);
                labels.push_back(group.empty() ? "[]" : "[" + group + "]");
                pos = semi + 1;
            }
        } else {
            // 默认 m=8 的几组 selective schedule
            schedules.push_back({});                        labels.push_back("[]");          // 无 FRO
            schedules.push_back({m_iter / 2});               labels.push_back("[mid]");        // 中间一次
            schedules.push_back({m_iter - 2});               labels.push_back("[late]");       // 末尾前一次
            schedules.push_back({m_iter / 2, m_iter - 2});   labels.push_back("[mid,late]");
            schedules.push_back({m_iter / 4, m_iter / 2, 3 * m_iter / 4});
                                                            labels.push_back("[1/4,1/2,3/4]");
            std::set<int> all_steps;
            for (int i = 0; i < m_iter - 1; ++i) all_steps.insert(i);
            schedules.push_back(all_steps);                  labels.push_back("[full FRO]");
        }

        std::cout << "=== Selective FRO 扫描（d=" << d << " m=" << m_iter << "） ===\n";
        std::cout << "schedule         sigma     λ₁_err%  λ₂_err%  λ₃_err%  λ₄_err%  "
                  << "cos₁    cos₂    cos₃    cos₄    R²(V)\n";
        std::cout << std::string(120, '-') << "\n";

        for (size_t s = 0; s < schedules.size(); ++s) {
            for (double sigma : sigma_list) {
                std::vector<double> err_acc(K, 0.0);
                std::vector<double> cos_acc(K, 0.0);
                double r2v_acc = 0.0;

                for (int trial = 0; trial < n_trials; ++trial) {
                    SelectiveLanczosResult lr = runSelectiveFROLanczos(
                        client.covMatrix(), d, v0, m_iter, schedules[s], sigma,
                        1000u + static_cast<unsigned>(trial));
                    ReconstructResult rec = reconstructFromTV(lr.T, lr.V_total, K);

                    for (int i = 0; i < K; ++i) {
                        double he = (i < rec.ritz_values.size())
                                      ? rec.ritz_values(i) * trace_C : 0.0;
                        double tv = (i < true_evals.size()) ? true_evals(i) : 0.0;
                        double pct = (std::abs(tv) > 1e-15)
                            ? std::abs(he - tv) / std::abs(tv) * 100.0 : 0.0;
                        err_acc[i] += pct;
                        double cs = (i < rec.eigenvectors.cols() && i < true_evecs_K.cols())
                            ? cosineSimilarity(rec.eigenvectors.col(i), true_evecs_K.col(i))
                            : 0.0;
                        cos_acc[i] += cs;
                    }

                    if (rec.eigenvectors.cols() == K && true_evecs_K.cols() >= K) {
                        Eigen::MatrixXd V_aligned(rec.eigenvectors.rows(), K);
                        for (int i = 0; i < K; ++i) {
                            double sgn = (true_evecs_K.col(i).dot(rec.eigenvectors.col(i)) >= 0)
                                ? 1.0 : -1.0;
                            V_aligned.col(i) = sgn * rec.eigenvectors.col(i);
                        }
                        double num = (V_aligned - true_evecs_K.leftCols(K)).squaredNorm();
                        double den = true_evecs_K.leftCols(K).squaredNorm();
                        r2v_acc += (den > 1e-15) ? (1.0 - num / den) : 0.0;
                    }
                }

                std::cout << std::left << std::setw(16) << labels[s]
                          << std::scientific << std::setprecision(1) << sigma
                          << std::right;
                for (int i = 0; i < 4; ++i) {
                    char buf[16];
                    if (i < K) std::snprintf(buf, sizeof(buf), " %7.2f%% ", err_acc[i] / n_trials);
                    else std::snprintf(buf, sizeof(buf), "  ----- ");
                    std::cout << buf;
                }
                for (int i = 0; i < 4; ++i) {
                    std::cout << std::fixed << std::setprecision(3)
                              << "  " << ((i < K) ? cos_acc[i] / n_trials : 0.0);
                }
                std::cout << "  " << std::setprecision(4) << r2v_acc / n_trials << "\n";
            }
            std::cout << "\n";
        }

        std::cout << "FRO 触发次数对应深度成本（HE 下每次 reorth ≈ 4 层）：\n";
        for (size_t s = 0; s < schedules.size(); ++s) {
            int total_reorth_cost = 0;
            for (int step : schedules[s]) total_reorth_cost += (step + 1);
            std::cout << "  " << std::left << std::setw(20) << labels[s]
                      << "触发 " << schedules[s].size() << " 次, "
                      << "累计 reorth 内积数 = " << total_reorth_cost
                      << " (≈ " << total_reorth_cost * 4 << " 层)\n";
        }
        return 0;
    }

    // 默认模式：原始 PRO 扫描
    printHeader();
    for (double sigma : sigma_list) {
        for (int b : b_list) {
            RunRecord r = runOne(
                client, v0, m_iter, K, sigma, b, n_trials,
                true_evals, true_evecs_K, trace_C);
            printRow(r);
        }
        std::cout << "\n";
    }

    std::cout << "\n说明:\n";
    std::cout << "  σ = 单步乘法相对噪声 (matvec 内部累加 √d 倍 → 等效 σ·√" << d << "≈" << std::scientific
              << std::setprecision(1) << (sigma_list.empty() ? 0.0 : sigma_list.back()) * std::sqrt(d) << ")\n";
    std::cout << "  b = 重正交化窗口大小 (0=不做, 2=Local PRO, " << m_iter << "=完整 FRO)\n";
    std::cout << "  R²(V) = 主成分方向相似度，越接近 1 越好；负数表示比随机基还差\n";
    std::cout << "  对每组配置取 " << n_trials << " 次不同噪声 seed 平均\n";
    return 0;
}
