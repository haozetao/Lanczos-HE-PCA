#include <cmath>
#include <iomanip>
#include <iostream>

#include <Eigen/Dense>

#include "client.h"
#include "server.h"

// 计算两个向量的余弦相似度 (取绝对值, 因为特征向量的符号可能相反)
static double cosineSimilarity(const Eigen::VectorXd& a,
                               const Eigen::VectorXd& b)
{
    double dot = a.dot(b);
    double na  = a.norm();
    double nb  = b.norm();
    if (na < 1e-15 || nb < 1e-15) return 0.0;
    return std::abs(dot) / (na * nb);
}

int main()
{
    // ====================================================================
    // 参数设定
    // ====================================================================
    constexpr int d = 10;   // 数据维度
    constexpr int p = 2;    // Block 块大小
    constexpr int m = 3;    // Lanczos 迭代次数
    constexpr int K = 2;    // 提取前 K 个主成分

    std::cout << "=== HE Block Lanczos PCA PoC ===" << std::endl;
    std::cout << "Parameters: d=" << d << ", p=" << p
              << ", m=" << m << ", K=" << K << std::endl;
    std::cout << std::string(50, '-') << std::endl;

    // ====================================================================
    // Phase 1: 初始化 (Client)
    // ====================================================================
    std::cout << "\n[Phase 1] 初始化..." << std::endl;

    Client client(d, p, m);

    // 1. 生成协方差矩阵
    client.generateCovarianceMatrix();
    std::cout << "  协方差矩阵 C (" << d << "x" << d << ") 已生成" << std::endl;

    // 2. 行打包加密 C → d 个密文
    auto enc_C = client.encryptCovMatrix();
    std::cout << "  加密 C: " << enc_C.size() << " 个密文 (行打包)" << std::endl;

    // 3. 生成初始随机块向量 V1 (d×p), QR 正交归一化
    Eigen::MatrixXd V1 = client.generateInitialBlock();
    client.vHistory().push_back(V1);
    std::cout << "  V1 (" << d << "x" << p << ") 正交归一化完成" << std::endl;

    // 4. 列打包加密 V1 → p 个密文
    auto enc_V = client.encryptBlockVec(V1);
    std::cout << "  加密 V1: " << enc_V.size() << " 个密文 (列打包)" << std::endl;

    // 5. 创建 Server, 共享上下文与公钥/计算密钥
    Server server(client.context(),
                  client.publicKey(),
                  client.relinKeys(),
                  client.galoisKeys());
    std::cout << "  Server 初始化完成" << std::endl;

    // ====================================================================
    // Phase 2: Block Lanczos 迭代 (j = 1 .. m)
    // ====================================================================
    std::cout << "\n[Phase 2] Block Lanczos 迭代 (" << m << " 轮)..."
              << std::endl;

    for (int j = 1; j <= m; ++j) {
        std::cout << "\n  --- 迭代 " << j << "/" << m << " ---" << std::endl;

        // ── Step 2.1: Server 密态矩阵乘 ────────────────────────────────
        // enc_W = enc_C * enc_V  (d*p 个标量密文)
        std::cout << "  [Server] 计算密态矩阵乘 enc_C * enc_V ..." << std::endl;
        auto enc_W = server.matmul(enc_C, enc_V, d);
        std::cout << "  [Server] 完成: " << enc_W.size() << " 个点积密文"
                  << std::endl;

        // ── Step 2.2: Client 解密 ──────────────────────────────────────
        Eigen::MatrixXd W_raw = client.decryptToMatrix(enc_W, d, p);
        std::cout << "  [Client] 解密得到 W_raw (" << d << "x" << p << ")"
                  << std::endl;

        // ── Step 2.3: Lanczos 校正 (明文) ──────────────────────────────
        // W_j = C*V_j - V_{j-1} * B_{j-1}
        Eigen::MatrixXd W_j = W_raw;
        if (j > 1) {
            const auto& V_prev = client.vHistory()[j - 2]; // V_{j-1}
            const auto& B_prev = client.bList()[j - 2];    // B_{j-1}
            W_j -= V_prev * B_prev;
        }

        // A_j = V_j^T * W_j  (p×p 投影系数矩阵)
        const auto& V_j = client.vHistory()[j - 1]; // V_j
        Eigen::MatrixXd A_j = V_j.transpose() * W_j;
        // 强制对称
        A_j = (A_j + A_j.transpose()) / 2.0;
        client.aList().push_back(A_j);

        std::cout << "  [Client] A_" << j << " (" << p << "x" << p
                  << ") 计算完成" << std::endl;

        // W_new = W_j - V_j * A_j
        Eigen::MatrixXd W_new = W_j - V_j * A_j;

        // ── Step 2.4: 完全正交化 + QR 分解 ────────────────────────────
        Eigen::MatrixXd W_hat =
            Client::fullReorthogonalize(W_new, client.vHistory());

        auto [V_next, B_j] = Client::qrDecompose(W_hat);

        client.bList().push_back(B_j);
        client.vHistory().push_back(V_next);

        std::cout << "  [Client] B_" << j << " (" << p << "x" << p
                  << "), V_" << (j + 1) << " (" << d << "x" << p
                  << ") 已提取" << std::endl;

        // ── Step 2.5: 重新加密 V_{j+1}, 深度归零 ──────────────────────
        enc_V = client.encryptBlockVec(V_next);
        std::cout << "  [Client] V_" << (j + 1) << " 重新加密完成 (深度重置)"
                  << std::endl;
    }

    // ====================================================================
    // Phase 3: 主成分提取与验证
    // ====================================================================
    std::cout << "\n[Phase 3] 主成分提取与验证..." << std::endl;

    // 1. 构建块三对角矩阵 T_m  (m*p × m*p)
    Eigen::MatrixXd T = client.buildBlockTridiag();
    std::cout << "  块三对角矩阵 T_m (" << T.rows() << "x" << T.cols()
              << ") 已构建" << std::endl;

    // 2. 对 T_m 做特征分解, 取前 K 个最大特征值
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_T(T);
    Eigen::VectorXd eig_vals_T = solver_T.eigenvalues().reverse();
    Eigen::MatrixXd eig_vecs_T = solver_T.eigenvectors().rowwise().reverse();
    Eigen::MatrixXd U_K = eig_vecs_T.leftCols(K); // (m*p) × K

    std::cout << "  T_m 特征分解完成" << std::endl;

    // 3. 拼接 V_total = [V_1, V_2, ..., V_m]  (d × m*p)
    //    (只使用前 m 个 V, 不含 V_{m+1})
    Eigen::MatrixXd V_total(d, m * p);
    for (int j = 0; j < m; ++j) {
        V_total.block(0, j * p, d, p) = client.vHistory()[j];
    }

    // 4. 近似特征向量: P = V_total * U_K  (d × K)
    Eigen::MatrixXd P = V_total * U_K;
    // 归一化每一列
    for (int k = 0; k < K; ++k) {
        P.col(k).normalize();
    }

    // 5. 真实特征向量 (直接对 C 做特征分解)
    Eigen::MatrixXd true_vecs = client.trueTopEigenvectors(K);

    // 真实特征值
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver_C(client.covMatrix());
    Eigen::VectorXd true_vals = solver_C.eigenvalues().reverse();

    // 6. 打印结果与余弦相似度对比
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "  结果对比 (前 " << K << " 个主成分)" << std::endl;
    std::cout << std::string(50, '=') << std::endl;

    std::cout << std::fixed << std::setprecision(4);
    for (int k = 0; k < K; ++k) {
        double lanczos_val = eig_vals_T(k);
        double true_val    = true_vals(k);
        double cosim = cosineSimilarity(P.col(k), true_vecs.col(k));

        std::cout << "  PC" << (k + 1) << ":"
                  << "  Lanczos 特征值 = " << lanczos_val
                  << "  |  真实特征值 = " << true_val
                  << "  |  余弦相似度 = " << cosim
                  << std::endl;
    }

    std::cout << "\n=== PoC 完成 ===" << std::endl;
    return 0;
}
