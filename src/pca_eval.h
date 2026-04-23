#pragma once

#include <Eigen/Dense>

// PCA 评估指标（参考 Panda 2021, Ma 2023 SOTA）
namespace pca_eval {

// R²(X): 重建得分 = 1 - ||X_c - X_c·V_K·V_K^T||_F² / ||X_c||_F²
// 范围 (-∞, 1]，越接近 1 越好；> 0.3 合格，> 0.5 优秀
// 输入: X_centered ∈ R^{N×d}, V_K ∈ R^{d×K}（主成分列向量，已归一化）
inline double reconstructionR2(const Eigen::MatrixXd& X_centered,
                               const Eigen::MatrixXd& V_K)
{
    if (V_K.cols() == 0) return 0.0;
    Eigen::MatrixXd proj = X_centered * V_K;            // N × K
    Eigen::MatrixXd rec = proj * V_K.transpose();       // N × d
    double ss_res = (X_centered - rec).squaredNorm();
    double ss_tot = X_centered.squaredNorm();
    if (ss_tot < 1e-15) return 0.0;
    return 1.0 - ss_res / ss_tot;
}

// R²(V): 主成分方向与 ground truth 的相似度
//   = 1 - ||V_enc·diag(sign) - V_true||_F² / ||V_true||_F²
// 自动对齐每列符号（PCA 特征向量方向任意取正负都合法）
inline double principalComponentR2(const Eigen::MatrixXd& V_enc,
                                   const Eigen::MatrixXd& V_true)
{
    int K = std::min(static_cast<int>(V_enc.cols()),
                     static_cast<int>(V_true.cols()));
    if (K == 0) return 0.0;
    Eigen::MatrixXd V_aligned = V_enc.leftCols(K);
    Eigen::MatrixXd V_ref = V_true.leftCols(K);
    for (int i = 0; i < K; ++i) {
        if (V_aligned.col(i).dot(V_ref.col(i)) < 0) {
            V_aligned.col(i) *= -1.0;
        }
    }
    double ss_res = (V_aligned - V_ref).squaredNorm();
    double ss_tot = V_ref.squaredNorm();
    if (ss_tot < 1e-15) return 0.0;
    return 1.0 - ss_res / ss_tot;
}

// 累计方差解释率（明文参考）
// = Σ_{i=1..K} λ_i / Σ_{i=1..d} λ_i
inline double explainedVarianceRatio(const Eigen::VectorXd& eigenvalues, int K)
{
    if (K <= 0 || K > eigenvalues.size()) return 0.0;
    double total = eigenvalues.sum();
    if (total < 1e-15) return 0.0;
    double top = eigenvalues.head(K).sum();
    return top / total;
}

// 单列 cos_sim
inline double cosineSimilarity(const Eigen::VectorXd& a, const Eigen::VectorXd& b)
{
    double na = a.norm(), nb = b.norm();
    if (na < 1e-15 || nb < 1e-15) return 0.0;
    return std::abs(a.dot(b)) / (na * nb);
}

} // namespace pca_eval
