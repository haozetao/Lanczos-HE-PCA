// FHE-PCA (OpenFHE 版): 密态 Lanczos + Newton 归一化 + 特征向量重构
//
// 支持两种模式：
//   baseline : m_iter=2，禁用 Bootstrap，与旧 SEAL 版本对齐
//   bootstrap: m_iter=5，启用 CKKS Bootstrap，验证深度刷新
//
// 编译得到可执行文件 he_pca，通过命令行参数切换：
//   ./he_pca            # 默认 baseline
//   ./he_pca baseline
//   ./he_pca bootstrap

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>

#include <Eigen/Dense>

#include "asor.h"
#include "client.h"
#include "pca_eval.h"
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
    return pca_eval::cosineSimilarity(a, b);
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

static bool hasEnv(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v;
}

static uint32_t estimateLanczosDepth(int newton_steps, int m_iter,
                                     bool enable_fro, int fro_skip_first)
{
    // depth_probe.cpp 实测（OpenFHE CKKS FLEXIBLEAUTO）：
    // V -> V_next 的 critical path 为 4 + 4 * NewtonSteps（无 FRO）。
    // 开启 FRO 后，最坏迭代（iter=m-1）会做 (m-1 - fro_skip_first) 次重正交化，
    // 每次 reorth 在 W_new 关键路径上多消耗 2 层。
    int reorth_steps = 0;
    if (enable_fro) {
        reorth_steps = std::max(0, m_iter - 1 - std::max(0, fro_skip_first));
    }
    return static_cast<uint32_t>(
        4 + 4 * std::max(newton_steps, 1) + 2 * reorth_steps);
}

static double defaultGuessSafety(int d)
{
    // 小维度下安全因子过大会让 Newton 初值过低，反而降低精度；
    // 大维度下需要保守初值避免 z0 = y0 * sqrt(x) 越过 sqrt(3) cliff。
    if (d <= 32) return 1.0;
    if (d <= 128) return 2.0;
    if (d <= 256) return 4.0;
    if (d <= 784) return 8.0;
    return 16.0;
}

struct RunConfig {
    int d;
    int p;
    int m_iter;
    int K;
    int newton_iters;
    bool enable_bootstrap;
    uint32_t levels_after_bootstrap;
    const char* label;
    // 若 dataset_N > 0，使用低秩数据生成（R²(X) 可计算），否则沿用旧的随机协方差
    int dataset_N = 0;
    int true_rank = 0;
    double noise_sigma = 0.0;
    // HE 全重正交化开关
    bool enable_fro = false;
    int fro_skip_first = 2;
};

static int runOnce(const RunConfig& cfg)
{
    std::cout << "=== FHE-PCA [" << cfg.label << "] (d=" << cfg.d
              << ", m_iter=" << cfg.m_iter
              << ", Newton=" << cfg.newton_iters
              << ", Bootstrap=" << (cfg.enable_bootstrap ? "ON" : "OFF")
              << ", FRO=" << (cfg.enable_fro ? "ON" : "OFF");
    if (cfg.enable_fro) {
        std::cout << "(skip_first=" << cfg.fro_skip_first << ")";
    }
    std::cout << ") ===" << std::endl;

    // ── Phase 1: Client 初始化 ──
    std::cout << "\n[Phase 1] Client setup" << std::endl;
    auto tc0 = Clock::now();
    Client client(cfg.d, cfg.p, cfg.m_iter, cfg.enable_bootstrap, cfg.levels_after_bootstrap);
    if (cfg.dataset_N > 0) {
        std::cout << "  数据: 低秩合成集 N=" << cfg.dataset_N
                  << " rank=" << cfg.true_rank
                  << " noise=" << cfg.noise_sigma << std::endl;
        client.generateLowRankDataset(cfg.dataset_N, cfg.true_rank, cfg.noise_sigma);
    } else {
        std::cout << "  数据: 随机协方差 A^T·A + I" << std::endl;
        client.generateCovarianceMatrix();
    }
    auto tc1 = Clock::now();
    std::cout << "  CryptoContext + KeyGen 耗时: "
              << std::fixed << std::setprecision(1)
              << std::chrono::duration<double, std::milli>(tc1 - tc0).count()
              << " ms" << std::endl;
    std::cout << "  multiplicativeDepth = " << client.multiplicativeDepth()
              << "   numSlots = " << client.numSlots() << std::endl;

    Eigen::MatrixXd C_original = client.covMatrix();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> true_solver(C_original);
    Eigen::VectorXd true_evals = true_solver.eigenvalues().reverse();
    Eigen::MatrixXd true_evecs = true_solver.eigenvectors().rowwise().reverse();

    double trace_C = client.normalizeCovariance();
    std::cout << "  trace(C) = " << std::fixed << std::setprecision(4) << trace_C << std::endl;

    Eigen::VectorXd v0 = randomUnitVector(cfg.d);

    // 明文预模拟第一步 Lanczos: 获取精确的 ||W_new||² 作为 Newton 初始猜测
    const Eigen::MatrixXd& C_hat = client.covMatrix();
    Eigen::VectorXd w_sim = C_hat * v0;
    double alpha0_sim = v0.dot(w_sim);
    Eigen::VectorXd w_res = w_sim - alpha0_sim * v0;
    double norm_sq_sim = w_res.squaredNorm();

    // 安全因子：让 Newton 初始猜测 y_0 从"低于真值"一侧进入，
    // 保证 z_0 = y_0·√x 永远 < 1 < √3，Newton 单调收敛而非过冲
    // 环境变量 GUESS_SAFETY 覆盖默认值；未设置时按维度自动选择。
    double guess_safety = envDouble("GUESS_SAFETY", defaultGuessSafety(cfg.d));
    double ev_guess = norm_sq_sim * guess_safety;
    const bool use_per_iter_guess = envInt("PER_ITER_GUESS", 1) != 0;
    std::vector<double> per_iter_guesses;
    if (use_per_iter_guess) {
        per_iter_guesses = client.plaintextMirrorResidualNormSqGuesses(v0, cfg.m_iter);
        for (double& g : per_iter_guesses) {
            g = std::max(g * guess_safety, 1e-18);
        }
    }

    std::cout << "  plaintext sim: α_0=" << std::setprecision(6) << alpha0_sim
              << "  ||W||²=" << std::scientific << std::setprecision(4) << norm_sq_sim
              << "  guess(safety=" << std::fixed << std::setprecision(1) << guess_safety
              << ")=1/√x=" << std::setprecision(2)
              << 1.0 / std::sqrt(ev_guess) << std::endl;
    if (use_per_iter_guess && !per_iter_guesses.empty()) {
        auto [min_it, max_it] = std::minmax_element(
            per_iter_guesses.begin(), per_iter_guesses.end());
        std::cout << "  per-iter Newton guess: ON  count=" << per_iter_guesses.size()
                  << "  range=[" << std::scientific << std::setprecision(3)
                  << *min_it << ", " << *max_it << "]" << std::fixed << std::endl;
    } else {
        std::cout << "  per-iter Newton guess: OFF（使用首轮固定 guess）" << std::endl;
    }

    // ── Phase 2: Server 密态 Lanczos ──
    std::cout << "\n[Phase 2] Server HE-Lanczos" << std::endl;

    auto enc_C = client.encryptCovMatrix();
    auto enc_v = client.encryptColumnVector(v0);

    Server server(
        client.cryptoContext(), client.publicKey(),
        client.multiplicativeDepth(), client.bootstrapEnabled(),
        client.numSlots());

    // aSOR 开关：环境变量 ASOR_EPS、ASOR_ALPHA 控制；未设置则关闭
    // aSOR 在多步 Lanczos 中容易因 ‖Wᵢ‖² 飘忽导致过冲发散，默认关闭以优先精度
    // 示例：ASOR_EPS=0.9 ASOR_ALPHA=4 ./he_pca bootstrap 8 50
    std::vector<double> asor_k;
    const char* asor_eps_env = std::getenv("ASOR_EPS");
    const char* asor_alpha_env = std::getenv("ASOR_ALPHA");
    if (asor_eps_env && asor_alpha_env) {
        double eps = std::atof(asor_eps_env);
        double alpha = std::atof(asor_alpha_env);
        ASORParams asor = precomputeInvSqrtASOR(eps, alpha);
        int std_iters = standardNewtonIterations(eps, alpha);
        asor_k = asor.k_factors;
        std::cout << "  aSOR: " << asor.iterations
                  << " 轮（同精度标准 Newton " << std_iters << " 轮），ε="
                  << eps << " α=" << alpha << std::endl;
    } else {
        std::cout << "  aSOR: 关闭（标准 Newton " << cfg.newton_iters
                  << " 轮）；如需开启: ASOR_EPS=0.9 ASOR_ALPHA=4 ..." << std::endl;
    }

    Server::LanczosStats stats;
    auto t0 = Clock::now();
    Server::LanczosResult lr = server.lanczosIteration(
        enc_C, enc_v, cfg.d, cfg.p, cfg.m_iter, ev_guess, per_iter_guesses,
        cfg.newton_iters, asor_k, &stats,
        cfg.enable_fro, cfg.fro_skip_first);
    auto t1 = Clock::now();
    double server_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "  耗时: " << std::fixed << std::setprecision(1)
              << server_ms << " ms" << std::endl;
    if (cfg.enable_bootstrap) {
        std::cout << "  Bootstrap 次数: " << stats.bootstrap_count
                  << "   Bootstrap 累计耗时: " << std::setprecision(1)
                  << stats.bootstrap_ms_total << " ms" << std::endl;
    }
    std::cout << "  α: " << lr.alphas.size() << "  β: " << lr.betas.size()
              << "  V_all: " << lr.V_all.size() << std::endl;

    // ── Phase 3: Client 后处理 ──
    std::cout << "\n[Phase 3] Client post-processing" << std::endl;

    int m = static_cast<int>(lr.alphas.size());
    Eigen::MatrixXd T = client.buildTridiagonalFromEncrypted(lr.alphas, lr.betas, m);
    std::cout << "  T_" << m << " =\n" << T << std::endl;

    CWFilterResult cw = client.cullumWilloughbyFilter(T, cfg.K);
    int n_good = static_cast<int>(cw.good_eigenvalues.size());
    std::cout << "  CW 保留特征值: " << n_good << std::endl;

    Eigen::MatrixXd V_total = client.decryptLanczosVectors(lr.V_all);
    Eigen::MatrixXd U = client.reconstructEigenvectors(V_total, cw, cfg.K);

    // ── Phase 4: 验证 ──
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  结果验证 (反归一化: ×" << std::setprecision(2) << trace_C << ")" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::cout << "\n  真实特征值:";
    for (int i = 0; i < std::min(cfg.K + 1, cfg.d); ++i) {
        std::cout << "  λ_" << (i + 1) << "=" << std::setprecision(2) << true_evals(i);
    }
    std::cout << std::endl;

    int show = std::min(cfg.K, n_good);
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

    // ── Phase 5: 学界对齐的评估指标 ──
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  [评估指标] 对齐 Panda 2021 / Ma 2023" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    // 反归一化后的 HE 特征值
    Eigen::VectorXd he_evals = cw.good_eigenvalues * trace_C;
    int K_use = std::min<int>(cfg.K, std::min<int>(U.cols(), he_evals.size()));

    // 明文特征向量（取前 K_use 列）
    Eigen::MatrixXd V_true_K = true_evecs.leftCols(std::min<int>(K_use, true_evecs.cols()));
    Eigen::MatrixXd V_enc_K = U.leftCols(K_use);

    // R²(V): 主成分方向相似度
    double r2v = pca_eval::principalComponentR2(V_enc_K, V_true_K);
    std::cout << "\n  R²(V) 主成分相似度 = " << std::fixed << std::setprecision(4) << r2v
              << "  （越接近 1 越好）" << std::endl;

    // R²(X): 重建评分（仅在有 X_c 时计算）
    if (client.hasData()) {
        const Eigen::MatrixXd& Xc = client.centeredData();
        double r2x_enc = pca_eval::reconstructionR2(Xc, V_enc_K);
        double r2x_true = pca_eval::reconstructionR2(Xc, V_true_K);
        std::cout << "  R²(X) 重建评分（密文 K=" << K_use << "）: "
                  << std::setprecision(4) << r2x_enc << std::endl;
        std::cout << "  R²(X) 重建评分（明文 K=" << K_use << "）: "
                  << std::setprecision(4) << r2x_true << std::endl;
        std::cout << "  R²(X) 差距 = " << std::setprecision(4)
                  << (r2x_true - r2x_enc) << "  （越小越好）" << std::endl;
    } else {
        std::cout << "  R²(X) 未计算（旧协方差数据源，无 X_c）" << std::endl;
    }

    // 累计方差解释率（明文参考）
    double evr_true = pca_eval::explainedVarianceRatio(true_evals, K_use);
    std::cout << "  累计方差解释率（明文，前 " << K_use << "）= "
              << std::setprecision(4) << evr_true << std::endl;

    std::cout << "\n  Server 耗时: " << std::setprecision(1) << server_ms << " ms" << std::endl;
    std::cout << "\n=== 完成 ===\n" << std::endl;
    return 0;
}

int main(int argc, char** argv)
{
    std::string mode = (argc >= 2) ? argv[1] : "baseline";

    if (mode == "baseline") {
        RunConfig cfg{
            /*d*/ 10,
            /*p*/ 1,
            /*m_iter*/ 2,
            /*K*/ 3,
            /*newton_iters*/ 2,
            /*enable_bootstrap*/ false,
            /*levels_after_bootstrap*/ 19,
            /*label*/ "baseline"};
        return runOnce(cfg);
    }

    if (mode == "bootstrap") {
        // 默认 m_iter 提升到 8 以让 HE-FRO 真正发挥提取多个特征的优势
        int m_iter = (argc >= 3) ? std::atoi(argv[2]) : 8;
        int d      = (argc >= 4) ? std::atoi(argv[3]) : 10;
        if (m_iter < 1) m_iter = 8;
        if (d < 2) d = 10;
        // 环境变量启用低秩数据集（可计算 R²(X)）
        int dataset_N = envInt("DATASET_N", 0);
        int true_rank = envInt("TRUE_RANK", std::min(d, 5));
        double noise  = envDouble("NOISE_SIGMA", 0.1);
        int K         = envInt("K", 3);
        int newton_iters = envInt("NEWTON_ITERS", 2);
        if (newton_iters < 1) newton_iters = 2;

        // FRO 默认开启（保留环境变量 ENABLE_FRO=0 可关闭）
        bool enable_fro = envInt("ENABLE_FRO", 1) != 0;
        int fro_skip_first = envInt("FRO_SKIP_FIRST", 2);
        if (fro_skip_first < 0) fro_skip_first = 0;

        // 单步 Lanczos 关键路径深度（含 FRO 时按最坏 iter 估算）
        const uint32_t per_step_depth =
            estimateLanczosDepth(newton_iters, m_iter, enable_fro, fro_skip_first);
        // levels_after_bootstrap 至少要 per_step_depth + 余量，否则
        // 进入末尾迭代时即便刚 BS 过也会触发 OpenFHE depth-exhausted。
        const uint32_t default_levels_after_bootstrap =
            std::max<uint32_t>(14, per_step_depth + 4);
        uint32_t levels_after_bootstrap = static_cast<uint32_t>(
            envInt("LEVELS_AFTER_BOOTSTRAP",
                   static_cast<int>(default_levels_after_bootstrap)));

        std::cout << "  Lanczos 单步最大深度估算 = " << per_step_depth
                  << "  → levels_after_bootstrap = " << levels_after_bootstrap
                  << std::endl;

        if (!hasEnv("GUESS_SAFETY")) {
            std::cout << "  GUESS_SAFETY 未设置，将按 d=" << d
                      << " 自动使用 " << defaultGuessSafety(d) << std::endl;
        }

        RunConfig cfg{
            /*d*/ d,
            /*p*/ 1,
            /*m_iter*/ m_iter,
            /*K*/ K,
            /*newton_iters*/ newton_iters,
            /*enable_bootstrap*/ true,
            /*levels_after_bootstrap*/ levels_after_bootstrap,
            /*label*/ "bootstrap",
            /*dataset_N*/ dataset_N,
            /*true_rank*/ true_rank,
            /*noise_sigma*/ noise,
            /*enable_fro*/ enable_fro,
            /*fro_skip_first*/ fro_skip_first};
        return runOnce(cfg);
    }

    std::cerr << "未知模式: " << mode << "\n用法: " << argv[0]
              << " [baseline|bootstrap [m_iter [d]]]" << std::endl;
    return 1;
}
