// 方向2（DIRECTION2_REFACTOR_PLAN）：纯 Server 密态 Lanczos + Client 解密后 CW 过滤
//
// 可行性说明：
// - 无 Bootstrapping 时，CKKS 乘法深度有限；32768 环下 coeff 链长度由 MaxBitCount 约束。
// - Newton 迭代次数压低，是因为每轮含多次密文乘+rescale，会吃模链；不是数学上只能 2～3 轮。
//   若接入 CKKS 自举（SEAL 无内置实现，常见为 OpenFHE 或论文级电路），可在刷新深度后提高 newton_iters / m_iter。
// - 单步 Lanczos 消耗大量层级；无边自举时 m_iter=2 与当前电路组合易 end of modulus switching chain。
// - p=1（标准三对角）；p>1 的块 Lanczos 需进一步扩展 Server。

#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>

#include <Eigen/Dense>

#include "client.h"
#include "server.h"

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

int main()
{
    // 协方差矩阵 C 为 d×d（Client: C = A^T A + I，A 元素 i.i.d. 标准正态）。
    constexpr int d = 10;
    constexpr int p = 1;
    // 无边自举：m_iter=2 在当前 Lanczos+Newton 深度下会耗尽模链；有自举后再调大。
    constexpr int m_iter = 1;
    // 无边自举：4 轮 Newton 易在末轮 rescale 时耗尽链；自举刷新后可尝试提高到 4～8 等。
    constexpr int newton_iters = 3;
    constexpr int K = 2;

    std::cout << "=== HE-Lanczos PCA (方向2: 纯 Server + CW) ===" << std::endl;
    std::cout << "d=" << d << ", p=" << p << ", m_iter=" << m_iter
              << ", newton_iters=" << newton_iters << ", K=" << K << std::endl;
    std::cout << std::string(55, '-') << std::endl;

    Client client(d, p, m_iter);
    client.generateCovarianceMatrix();

    auto enc_C = client.encryptCovMatrix();
    Eigen::VectorXd v0 = randomUnitVector(d);
    auto enc_v = client.encryptColumnVector(v0);

    double ev_guess = client.eigenvalueMagnitudeGuess();

    Server server(
        client.context(),
        client.publicKey(),
        client.relinKeys(),
        client.galoisKeys(),
        client.ckksScale());

    std::cout << "\n[Phase 1] 已发送 enc_C 与 enc_v0 至 Server。" << std::endl;

    std::cout << "\n[Phase 2] Server 密态 Lanczos（单轮交互）..." << std::endl;
    Server::LanczosResult lr = server.lanczosIteration(
        enc_C, enc_v, d, p, m_iter, ev_guess, newton_iters);

    std::cout << "  返回 α 密文: " << lr.alphas.size()
              << " ，β 密文: " << lr.betas.size() << std::endl;

    int m = static_cast<int>(lr.alphas.size());
    Eigen::MatrixXd T = client.buildTridiagonalFromEncrypted(
        lr.alphas, lr.betas, m);

    std::cout << "\n[Phase 3] Client 解密构造 T_" << m << "，并做 CW 过滤..." << std::endl;
    std::cout << "  T_m =\n" << T << std::endl;

    CWFilterResult cw = client.cullumWilloughbyFilter(T, K);
    int n_found = static_cast<int>(cw.good_eigenvalues.size());
    std::cout << "  过滤后保留特征值个数: " << n_found << std::endl;

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> true_solver(client.covMatrix());
    Eigen::VectorXd true_evals = true_solver.eigenvalues().reverse();

    std::cout << "\n" << std::string(55, '=') << std::endl;
    std::cout << "  特征值对比（Ritz / CW vs 真实 C 的前若干大特征值）" << std::endl;
    std::cout << std::string(55, '=') << std::endl;
    std::cout << std::fixed << std::setprecision(4);

    int show = std::min(n_found, K);
    for (int i = 0; i < show; ++i) {
        double he_val = cw.good_eigenvalues(i);
        double tv = true_evals(i);
        double err_pct = tv != 0.0 ? std::abs(he_val - tv) / std::abs(tv) * 100.0 : 0.0;
        std::cout << "  #" << (i + 1) << "  Lanczos/CW: " << he_val
                  << "  |  真实: " << tv
                  << "  |  相对误差: " << err_pct << "%" << std::endl;
    }

    if (m_iter < K) {
        std::cout << "\n  提示: m_iter < K 时 Krylov 子空间维度过小，"
                  << "Ritz 值不一定逼近前 K 个主特征值；增大 m_iter（并控制深度）。" << std::endl;
    }

    std::cout << "\n=== 完成 ===" << std::endl;
    return 0;
}
