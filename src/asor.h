#pragma once

#include <cmath>
#include <stdexcept>
#include <vector>

// aSOR (adaptive Successive Over-Relaxation) 松弛因子预计算
//
// 论文: "Adaptive Successive Over-Relaxation Method for a Faster Iterative
//        Approximation of Homomorphic Operations" (2024)
//
// 对 Newton-Raphson 1/sqrt(x) 迭代 f(z) = z*(3-z^2)/2，
// 预计算最优松弛因子 k_i 使收敛加速约 2 倍。

struct ASORParams {
    std::vector<double> k_factors;
    int iterations;
    double epsilon_final;
};

// 对 f(z) = z*(3-z^2)/2 预计算松弛因子序列。
//
// epsilon: 输入范围下界（z_1 >= epsilon），推荐取保守值 0.001
// alpha:   目标精度，迭代直到 1 - epsilon_i < 2^{-alpha}
//
// 解析解: k_i = sqrt(3 / (1 + eps_i + eps_i^2))
//         eps_{i+1} = k_i * (3 - k_i^2) / 2
inline ASORParams precomputeInvSqrtASOR(double epsilon, double alpha)
{
    if (epsilon <= 0.0 || epsilon >= 1.0) {
        throw std::invalid_argument("precomputeInvSqrtASOR: epsilon must be in (0, 1)");
    }
    if (alpha <= 0.0) {
        throw std::invalid_argument("precomputeInvSqrtASOR: alpha must be positive");
    }

    const double target = std::pow(2.0, -alpha);
    ASORParams params;
    double eps = epsilon;

    constexpr int kMaxIters = 256;
    for (int i = 0; i < kMaxIters; ++i) {
        if (1.0 - eps < target) {
            break;
        }
        double k = std::sqrt(3.0 / (1.0 + eps + eps * eps));
        params.k_factors.push_back(k);
        eps = k * (3.0 - k * k) / 2.0;
    }

    params.iterations = static_cast<int>(params.k_factors.size());
    params.epsilon_final = eps;
    return params;
}

// 不带 aSOR 时，标准 Newton 需要的迭代次数（作为对照基线）
inline int standardNewtonIterations(double epsilon, double alpha)
{
    const double target = std::pow(2.0, -alpha);
    double eps = epsilon;
    int count = 0;
    constexpr int kMaxIters = 256;
    for (int i = 0; i < kMaxIters; ++i) {
        if (1.0 - eps < target) {
            break;
        }
        // f(eps) = eps * (3 - eps^2) / 2, 无松弛 (k=1)
        eps = eps * (3.0 - eps * eps) / 2.0;
        ++count;
    }
    return count;
}
