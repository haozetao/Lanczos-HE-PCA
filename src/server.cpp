#include "server.h"

#include <iostream>
#include <stdexcept>

// ============================================================================
// 构造函数
// ============================================================================
Server::Server(std::shared_ptr<seal::SEALContext> ctx,
               const seal::PublicKey&  pk,
               const seal::RelinKeys&  rlk,
               const seal::GaloisKeys& glk)
    : context_(std::move(ctx))
    , evaluator_(*context_)
    , encoder_(*context_)
    , relin_keys_(rlk)
    , galois_keys_(glk)
{
}

// ============================================================================
// innerProduct —— 两个向量密文的内积
//
// 输入: a, b — 前 d 个 slot 存储向量分量, 其余 slot 为 0
// 输出: 一个密文, 其 slot 0 存储 sum_k a[k]*b[k]
//
// 实现:
//   1. element-wise multiply → 得到 [a0*b0, a1*b1, ..., a_{d-1}*b_{d-1}, 0...]
//   2. relinearize → 将 3-元组密文缩减回 2-元组
//   3. rescale → scale 从 2^80 降至 ~2^40
//   4. rotate-and-sum → 步长 1,2,4,8 逐步折叠, slot 0 累加全部分量
//      (d=10, 需要覆盖 16 个位置; slot 10..15 本身为 0, 不影响结果)
// ============================================================================
seal::Ciphertext Server::innerProduct(
    const seal::Ciphertext& a,
    const seal::Ciphertext& b) const
{
    seal::Ciphertext product;
    evaluator_.multiply(a, b, product);
    evaluator_.relinearize_inplace(product, relin_keys_);
    evaluator_.rescale_to_next_inplace(product);

    // rotate-and-sum: 将所有 slot 的值累加到 slot 0
    seal::Ciphertext rotated;
    for (int step : {1, 2, 4, 8}) {
        evaluator_.rotate_vector(product, step, galois_keys_, rotated);
        evaluator_.add_inplace(product, rotated);
    }

    return product;
}

// ============================================================================
// matmul —— 密态矩阵乘法
//
// enc_C: d 个密文 (行打包), enc_C[i] = Encrypt([C[i][0], ..., C[i][d-1]])
// enc_V: p 个密文 (列打包), enc_V[j] = Encrypt([V[0][j], ..., V[d-1][j]])
//
// 返回: d*p 个标量密文 (行优先)
//   result[i*p + j].slot[0] = dot(C_row_i, V_col_j) = (C*V)[i][j]
// ============================================================================
std::vector<seal::Ciphertext> Server::matmul(
    const std::vector<seal::Ciphertext>& enc_C,
    const std::vector<seal::Ciphertext>& enc_V,
    int d) const
{
    int p = static_cast<int>(enc_V.size());
    std::vector<seal::Ciphertext> result(d * p);

    for (int i = 0; i < d; ++i) {
        for (int j = 0; j < p; ++j) {
            result[i * p + j] = innerProduct(enc_C[i], enc_V[j]);
        }
    }

    return result;
}
