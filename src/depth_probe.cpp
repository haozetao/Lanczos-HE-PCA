// 层数验证：打印各种 OpenFHE CKKS 操作前后的 GetLevel()，
// 用最直白的方式确认到底每种乘法消耗多少层。
//
// 编译：CMake target depth_probe（见 CMakeLists.txt）
// 运行：./build/depth_probe
//
// 重点验证：
//   1. ct × ct
//   2. ct × Plaintext（packed mask）
//   3. ct × double 标量
//   4. EvalSub(double, ct)
//   5. EvalAdd(ct, ct)
//   6. EvalRotate(ct)
//   7. Newton 1/√x 单步
//   8. innerProduct (rotate-and-sum)
//   9. lanczos critical path 串联

#include <iomanip>
#include <iostream>
#include <vector>

#include "openfhe.h"

using namespace lbcrypto;

static void show(const std::string& label, const Ciphertext<DCRTPoly>& ct)
{
    std::cout << "  " << std::left << std::setw(48) << label
              << "  level=" << std::right << std::setw(2) << ct->GetLevel()
              << "  noiseScaleDeg=" << ct->GetNoiseScaleDeg()
              << "  scalingFactor=" << std::scientific << std::setprecision(2)
              << ct->GetScalingFactor() << std::endl;
}

int main()
{
    CCParams<CryptoContextCKKSRNS> params;
    params.SetSecretKeyDist(UNIFORM_TERNARY);
    params.SetSecurityLevel(HEStd_NotSet);
    params.SetRingDim(1u << 14); // 16384 用更小 ring 加速测试
    params.SetScalingTechnique(FLEXIBLEAUTO);
    params.SetScalingModSize(59);
    params.SetFirstModSize(60);
    params.SetMultiplicativeDepth(20);

    auto cc = GenCryptoContext(params);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    auto keys = cc->KeyGen();
    cc->EvalMultKeyGen(keys.secretKey);
    cc->EvalRotateKeyGen(keys.secretKey, {1, 2, 4});

    uint32_t slots = cc->GetRingDimension() / 2;
    std::vector<double> v(slots, 0.0);
    for (uint32_t i = 0; i < 8; ++i) v[i] = 0.1 * (i + 1);

    auto pt_v = cc->MakeCKKSPackedPlaintext(v);
    auto ct = cc->Encrypt(keys.publicKey, pt_v);

    std::cout << "\n=== 1. 基础原子操作的层数消耗 ===\n" << std::endl;
    show("初始 ct (fresh)", ct);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n[A] ct × ct" << std::endl;
    auto a = ct;
    show("  before EvalMult(ct, ct)", a);
    auto a2 = cc->EvalMult(a, a);
    show("  after  EvalMult(ct, ct)", a2);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n[B] ct × Plaintext (packed mask)" << std::endl;
    std::vector<double> mask(slots, 0.0);
    mask[3] = 1.0;
    auto mask_pt = cc->MakeCKKSPackedPlaintext(mask);
    show("  before EvalMult(ct, mask_pt)", a);
    auto am = cc->EvalMult(a, mask_pt);
    show("  after  EvalMult(ct, mask_pt)", am);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n[C] ct × double 标量 (例如 *0.5)" << std::endl;
    show("  before EvalMult(ct, 0.5)", a);
    auto ah = cc->EvalMult(a, 0.5);
    show("  after  EvalMult(ct, 0.5)", ah);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n[D] EvalSub(double, ct) — 例如 3.0 − ct" << std::endl;
    show("  before EvalSub(3.0, ct)", a);
    auto as = cc->EvalSub(3.0, a);
    show("  after  EvalSub(3.0, ct)", as);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n[E] EvalAdd(ct, ct)" << std::endl;
    auto aa = cc->EvalAdd(a, a);
    show("  after  EvalAdd(ct, ct)", aa);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n[F] EvalRotate(ct, 1)" << std::endl;
    auto ar = cc->EvalRotate(a, 1);
    show("  after  EvalRotate(ct, 1)", ar);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 2. innerProduct rotate-and-sum 是否消耗层 ===\n" << std::endl;
    auto product = cc->EvalMult(ct, ct);
    show("初始 product = ct·ct", product);
    for (uint32_t step = 1; step <= 4; step *= 2) {
        auto rotated = cc->EvalRotate(product, static_cast<int32_t>(step));
        product = cc->EvalAdd(product, rotated);
        show("  rotate-and-add step=" + std::to_string(step), product);
    }

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 3. Newton 1/√x 单步真实层数 ===\n" << std::endl;
    auto x_ct = ct;
    auto y_ct = ct; // 用同样的 ct 模拟 y
    show("x_ct (fresh)", x_ct);
    show("y_ct (fresh)", y_ct);

    auto y_sq = cc->EvalMult(y_ct, y_ct);
    show("  step1: y_sq = y·y", y_sq);
    auto x_y_sq = cc->EvalMult(x_ct, y_sq);
    show("  step2: x_y_sq = x·y_sq", x_y_sq);
    auto t = cc->EvalSub(3.0, x_y_sq);
    show("  step3: t = 3 − x_y_sq", t);
    auto yt = cc->EvalMult(y_ct, t);
    show("  step4: yt = y · t", yt);
    auto out = cc->EvalMult(yt, 0.5);
    show("  step5: out = yt · 0.5", out);
    std::cout << "\n  ⇒ Newton 单步使 y 从 level " << y_ct->GetLevel()
              << " 增长到 level " << out->GetLevel() << std::endl;

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 4. Newton 两轮 ===\n" << std::endl;
    auto y2 = ct;
    show("y0 (fresh)", y2);
    for (int it = 0; it < 2; ++it) {
        auto ys = cc->EvalMult(y2, y2);
        auto xys = cc->EvalMult(x_ct, ys);
        auto tt = cc->EvalSub(3.0, xys);
        auto yt2 = cc->EvalMult(y2, tt);
        y2 = cc->EvalMult(yt2, 0.5);
        show("  after Newton iter " + std::to_string(it + 1), y2);
    }
    auto sqrt_val = cc->EvalMult(x_ct, y2);
    show("  sqrt_val = x · y2", sqrt_val);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 5. Lanczos critical path（newton=2，模拟）===\n" << std::endl;
    auto V = ct;
    auto C_row = ct; // 假装是 enc_C[i]
    show("V (fresh)", V);

    // Step 1: scalars[i] = innerProduct(C[i], V) (省略 rotate-and-sum，只计 mul)
    auto scalar = cc->EvalMult(C_row, V);
    show("  step1a: scalars[i] = mul(C, V)", scalar);
    // Step 1: pack mask
    auto Wvec = cc->EvalMult(scalar, mask_pt);
    show("  step1b: Wvec = mask · scalar", Wvec);

    // Step 2: alpha = innerProduct(V, Wvec)
    auto alpha = cc->EvalMult(V, Wvec);
    show("  step2: alpha = V · Wvec", alpha);

    // Step 3: alphaV = scalarVecMult(alpha, V); Wnew = Wvec − alphaV
    auto alphaV = cc->EvalMult(alpha, V);
    show("  step3a: alphaV = alpha · V", alphaV);
    auto Wnew = cc->EvalSub(Wvec, alphaV);
    show("  step3b: Wnew = Wvec − alphaV", Wnew);

    // Step 4: norm_sq = innerProduct(Wnew, Wnew)
    auto norm_sq = cc->EvalMult(Wnew, Wnew);
    show("  step4: norm_sq = Wnew · Wnew", norm_sq);

    // Step 5: Newton(norm_sq, 2)
    auto y3 = ct;
    for (int it = 0; it < 2; ++it) {
        auto ys = cc->EvalMult(y3, y3);
        auto xys = cc->EvalMult(norm_sq, ys);
        auto tt = cc->EvalSub(3.0, xys);
        auto yt2 = cc->EvalMult(y3, tt);
        y3 = cc->EvalMult(yt2, 0.5);
    }
    show("  step5: y after Newton(2)", y3);
    auto sqrt2 = cc->EvalMult(norm_sq, y3);
    show("  step5b: sqrt = norm_sq · y", sqrt2);

    // Step 6: V_next = inv_sqrt × Wnew
    auto V_next = cc->EvalMult(y3, Wnew);
    show("  step6: V_next = inv_sqrt · Wnew", V_next);

    std::cout << "\n  ⇒ V 从 level " << V->GetLevel()
              << " 经过一步完整 Lanczos 后 V_next 在 level "
              << V_next->GetLevel() << std::endl;

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 6. 验证「首次乘法不消耗层」是否泛化 ===\n" << std::endl;
    std::cout << "[a] 一个 fresh ct 多次自乘 (chain mul):" << std::endl;
    auto chain = ct;
    show("  chain start", chain);
    for (int i = 0; i < 5; ++i) {
        chain = cc->EvalMult(chain, ct);
        show("  after mul " + std::to_string(i + 1), chain);
    }

    std::cout << "\n[b] 交替 EvalMult 与 EvalAdd（看 add 是否「重置 deg」）:" << std::endl;
    auto z = ct;
    show("  z (fresh)", z);
    z = cc->EvalMult(z, ct);
    show("  z = z · ct", z);
    z = cc->EvalAdd(z, z);
    show("  z = z + z", z);
    z = cc->EvalMult(z, ct);
    show("  z = z · ct", z);

    std::cout << "\n[c] 用一个已经 deg=2 的 ct 再去 × Plaintext：" << std::endl;
    auto u = cc->EvalMult(ct, ct);  // deg=2 lvl=0
    show("  u = ct·ct", u);
    auto u2 = cc->EvalMult(u, mask_pt);
    show("  u2 = u · mask_pt", u2);

    std::cout << "\n[d] 用一个已经 deg=2 的 ct 再去 × double 标量：" << std::endl;
    auto u3 = cc->EvalMult(u, 0.5);
    show("  u3 = u · 0.5", u3);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 7. Newton 三轮的层数 ===\n" << std::endl;
    auto y3a = ct;
    show("y0 (fresh)", y3a);
    for (int it = 0; it < 3; ++it) {
        auto ys = cc->EvalMult(y3a, y3a);
        auto xys = cc->EvalMult(x_ct, ys);
        auto tt = cc->EvalSub(3.0, xys);
        auto yt2 = cc->EvalMult(y3a, tt);
        y3a = cc->EvalMult(yt2, 0.5);
        show("  after Newton iter " + std::to_string(it + 1), y3a);
    }
    auto sqrt3 = cc->EvalMult(x_ct, y3a);
    show("  sqrt_val = x · y3", sqrt3);

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 8. Lanczos critical path with newton=3 ===\n" << std::endl;
    auto V8 = ct;
    show("V (fresh)", V8);
    auto sc = cc->EvalMult(C_row, V8);
    show("  scalars = C·V", sc);
    auto W8 = cc->EvalMult(sc, mask_pt);
    show("  Wvec = scalars·mask", W8);
    auto a8 = cc->EvalMult(V8, W8);
    show("  alpha = V·Wvec", a8);
    auto aV8 = cc->EvalMult(a8, V8);
    show("  alphaV = alpha·V", aV8);
    auto Wn8 = cc->EvalSub(W8, aV8);
    show("  Wnew = Wvec − alphaV", Wn8);
    auto ns8 = cc->EvalMult(Wn8, Wn8);
    show("  norm_sq = Wnew·Wnew", ns8);

    auto y8 = ct;
    for (int it = 0; it < 3; ++it) {
        auto ys = cc->EvalMult(y8, y8);
        auto xys = cc->EvalMult(ns8, ys);
        auto tt = cc->EvalSub(3.0, xys);
        auto yt2 = cc->EvalMult(y8, tt);
        y8 = cc->EvalMult(yt2, 0.5);
    }
    show("  y after Newton(3)", y8);
    auto V_next3 = cc->EvalMult(y8, Wn8);
    show("  V_next = y · Wnew", V_next3);

    std::cout << "\n  ⇒ Newton=3 时 V 从 level " << V8->GetLevel()
              << " 到 V_next 在 level " << V_next3->GetLevel() << std::endl;

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 9. Lanczos critical path with newton=1 ===\n" << std::endl;
    auto V9 = ct;
    auto sc9 = cc->EvalMult(C_row, V9);
    auto W9 = cc->EvalMult(sc9, mask_pt);
    auto a9 = cc->EvalMult(V9, W9);
    auto aV9 = cc->EvalMult(a9, V9);
    auto Wn9 = cc->EvalSub(W9, aV9);
    auto ns9 = cc->EvalMult(Wn9, Wn9);

    auto y9 = ct;
    for (int it = 0; it < 1; ++it) {
        auto ys = cc->EvalMult(y9, y9);
        auto xys = cc->EvalMult(ns9, ys);
        auto tt = cc->EvalSub(3.0, xys);
        auto yt2 = cc->EvalMult(y9, tt);
        y9 = cc->EvalMult(yt2, 0.5);
    }
    auto V_next1 = cc->EvalMult(y9, Wn9);
    show("  V_next (newton=1)", V_next1);
    std::cout << "  ⇒ Newton=1 时 V 从 0 到 V_next 在 level "
              << V_next1->GetLevel() << std::endl;

    // ────────────────────────────────────────────────────────────
    std::cout << "\n=== 10. 「明文乘是否真的免费」论文 vs 实测 ===\n" << std::endl;
    std::cout << "[i] 长链反复 ct × Plaintext（每次新 mask）：" << std::endl;
    auto chain_pt = ct;
    show("  start", chain_pt);
    for (int i = 0; i < 6; ++i) {
        std::vector<double> m(slots, 1.0);
        auto pt = cc->MakeCKKSPackedPlaintext(m);
        chain_pt = cc->EvalMult(chain_pt, pt);
        show("  after pt-mul " + std::to_string(i + 1), chain_pt);
    }

    std::cout << "\n[ii] 长链反复 ct × double 标量：" << std::endl;
    auto chain_d = ct;
    show("  start", chain_d);
    for (int i = 0; i < 6; ++i) {
        chain_d = cc->EvalMult(chain_d, 1.0);
        show("  after dbl-mul " + std::to_string(i + 1), chain_d);
    }

    std::cout << "\n[iii] 长链反复 ct × ct（同一 ct）做对照：" << std::endl;
    auto chain_c = ct;
    show("  start", chain_c);
    for (int i = 0; i < 6; ++i) {
        chain_c = cc->EvalMult(chain_c, ct);
        show("  after ct-mul " + std::to_string(i + 1), chain_c);
    }

    std::cout << "\n[iv] 仅 plaintext 加常数（不乘）的层数：" << std::endl;
    auto only_add = ct;
    show("  start", only_add);
    for (int i = 0; i < 5; ++i) {
        only_add = cc->EvalAdd(only_add, 1.0);
        show("  after EvalAdd(ct, 1.0) #" + std::to_string(i + 1), only_add);
    }

    return 0;
}
