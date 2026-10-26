const EON_PI : f32 = 3.14159265;
const EON_INV_PI : f32 = 1.0 / EON_PI;
const EON_CONSTANT1_FON : f32 = 0.5 - 2.0 / (3.0 * EON_PI);
const EON_CONSTANT2_FON : f32 = 2.0 / 3.0 - 28.0 / (15.0 * EON_PI);

fn eon_safe_normalize(vectorValue : vec3<f32>, fallbackValue : vec3<f32>) -> vec3<f32> {
    let vectorLength = length(vectorValue);
    return select(fallbackValue, vectorValue / vectorLength, vectorLength > 1.0e-6);
}

fn eon_E_FON_exact(muIn : f32, roughness : f32) -> f32 {
    let mu = clamp(muIn, 1.0e-4, 1.0);
    let AF = 1.0 / (1.0 + EON_CONSTANT1_FON * roughness);
    let BF = roughness * AF;
    let Si = sqrt(max(1.0 - mu * mu, 0.0));
    let G =
        Si * (acos(mu) - Si * mu) +
        (2.0 / 3.0) * ((Si / max(mu, 1.0e-4)) * (1.0 - Si * Si * Si) - Si);
    return AF + (BF * EON_INV_PI) * G;
}

fn eon_E_FON_approx(muIn : f32, roughness : f32) -> f32 {
    let mu = clamp(muIn, 0.0, 1.0);
    let mucomp = 1.0 - mu;
    let g1 = 0.0571085289;
    let g2 = 0.491881867;
    let g3 = -0.332181442;
    let g4 = 0.0714429953;
    let GoverPi = mucomp * (g1 + mucomp * (g2 + mucomp * (g3 + mucomp * g4)));
    return (1.0 + roughness * GoverPi) / (1.0 + EON_CONSTANT1_FON * roughness);
}

fn eon_f(rho : vec3<f32>, roughness : f32, wi_local : vec3<f32>, wo_local : vec3<f32>, exact : bool) -> vec3<f32> {
    let mu_i = clamp(wi_local.z, 0.0, 1.0);
    let mu_o = clamp(wo_local.z, 0.0, 1.0);
    let s = dot(wi_local, wo_local) - mu_i * mu_o;
    let sovertF = select(s, s / max(max(mu_i, mu_o), 1.0e-4), s > 0.0);
    let AF = 1.0 / (1.0 + EON_CONSTANT1_FON * roughness);
    let f_ss = (rho * EON_INV_PI) * AF * (1.0 + roughness * sovertF);
    let EFo = select(eon_E_FON_approx(mu_o, roughness), eon_E_FON_exact(mu_o, roughness), exact);
    let EFi = select(eon_E_FON_approx(mu_i, roughness), eon_E_FON_exact(mu_i, roughness), exact);
    let avgEF = AF * (1.0 + EON_CONSTANT2_FON * roughness);
    let rho_ms = (rho * rho) * avgEF / max(vec3<f32>(1.0e-7), vec3<f32>(1.0) - rho * (1.0 - avgEF));
    let f_ms =
        (rho_ms * EON_INV_PI) *
        max(vec3<f32>(1.0e-7), vec3<f32>(1.0 - EFo)) *
        max(vec3<f32>(1.0e-7), vec3<f32>(1.0 - EFi)) /
        max(vec3<f32>(1.0e-7), vec3<f32>(1.0 - avgEF));
    return f_ss + f_ms;
}

fn eon_E(rho : vec3<f32>, roughness : f32, wi_local : vec3<f32>, exact : bool) -> vec3<f32> {
    let mu_i = clamp(wi_local.z, 0.0, 1.0);
    let AF = 1.0 / (1.0 + EON_CONSTANT1_FON * roughness);
    let EF = select(eon_E_FON_approx(mu_i, roughness), eon_E_FON_exact(mu_i, roughness), exact);
    let avgEF = AF * (1.0 + EON_CONSTANT2_FON * roughness);
    let rho_ms = (rho * rho) * avgEF / max(vec3<f32>(1.0e-7), vec3<f32>(1.0) - rho * (1.0 - avgEF));
    return rho * EF + rho_ms * (1.0 - EF);
}

fn eon_ltc_coeffs(muIn : f32, roughness : f32) -> vec4<f32> {
    let mu = clamp(muIn, 0.0, 1.0);
    let a =
        1.0 + roughness * (0.303392 + (-0.518982 + 0.111709 * mu) * mu + (-0.276266 + 0.335918 * mu) * roughness);
    let b =
        roughness * (-1.16407 + 1.15859 * mu + (0.150815 - 0.150105 * mu) * roughness) /
        max(mu * mu * mu - 1.43545, -1.0e-4);
    let c = 1.0 + roughness * (0.20013 + (-0.506373 + 0.261777 * mu) * mu);
    let d =
        roughness * (0.540852 + (-1.01625 + 0.475392 * mu) * mu) /
        max(-1.0743 + (0.0725628 + mu) * mu, 1.0e-4);
    return vec4<f32>(a, b, c, d);
}

fn eon_orthonormal_basis_ltc(w : vec3<f32>) -> mat3x3<f32> {
    let lenSqr = dot(w.xy, w.xy);
    let X = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(w.x, w.y, 0.0) / sqrt(lenSqr), lenSqr > 0.0);
    let Y = vec3<f32>(-X.y, X.x, 0.0);
    return mat3x3<f32>(X, Y, vec3<f32>(0.0, 0.0, 1.0));
}

fn eon_cltc_sample(wo_local : vec3<f32>, roughness : f32, u1 : f32, u2 : f32) -> vec4<f32> {
    let coeffs = eon_ltc_coeffs(wo_local.z, roughness);
    let a = coeffs.x;
    let b = coeffs.y;
    let c = coeffs.z;
    let d = coeffs.w;
    let R = sqrt(clamp(u1, 0.0, 1.0));
    let phi = 2.0 * EON_PI * u2;
    var x = R * cos(phi);
    let y = R * sin(phi);
    let vz = 1.0 / sqrt(d * d + 1.0);
    let s = 0.5 * (1.0 + vz);
    x = -mix(sqrt(max(1.0 - y * y, 0.0)), x, s);
    let wh = vec3<f32>(x, y, sqrt(max(1.0 - (x * x + y * y), 0.0)));
    let pdf_wh = wh.z / (EON_PI * s);
    var wi = vec3<f32>(a * wh.x + b * wh.z, c * wh.y, d * wh.x + wh.z);
    let wiLen = length(wi);
    let detM = c * (a - b * d);
    let pdf_wi = pdf_wh * wiLen * wiLen * wiLen / max(detM, 1.0e-6);
    let fromLTC = eon_orthonormal_basis_ltc(wo_local);
    wi = eon_safe_normalize(fromLTC * wi, vec3<f32>(0.0, 0.0, 1.0));
    return vec4<f32>(wi, pdf_wi);
}

fn eon_cltc_pdf(wo_local : vec3<f32>, wi_local : vec3<f32>, roughness : f32) -> f32 {
    let toLTC = transpose(eon_orthonormal_basis_ltc(wo_local));
    let wi = toLTC * wi_local;
    let coeffs = eon_ltc_coeffs(wo_local.z, roughness);
    let a = coeffs.x;
    let b = coeffs.y;
    let c = coeffs.z;
    let d = coeffs.w;
    let detM = c * (a - b * d);
    let wh = vec3<f32>(c * (wi.x - b * wi.z), (a - b * d) * wi.y, -c * (d * wi.x - a * wi.z));
    let lenSqr = dot(wh, wh);
    let vz = 1.0 / sqrt(d * d + 1.0);
    let s = 0.5 * (1.0 + vz);
    return detM * detM / max(lenSqr * lenSqr, 1.0e-6) * max(wh.z, 0.0) / (EON_PI * s);
}

fn eon_uniform_lobe_sample(u1 : f32, u2 : f32) -> vec3<f32> {
    let sinTheta = sqrt(max(1.0 - u1 * u1, 0.0));
    let phi = 2.0 * EON_PI * u2;
    return vec3<f32>(sinTheta * cos(phi), sinTheta * sin(phi), u1);
}

fn eon_sample(wo_local : vec3<f32>, roughnessIn : f32, u1In : f32, u2In : f32) -> vec4<f32> {
    let roughness = clamp(roughnessIn, 0.0, 1.0);
    let mu = clamp(wo_local.z, 0.0, 1.0);
    let P_u = pow(max(roughness, 1.0e-6), 0.1) * (0.162925 + (-0.372058 + (0.538233 - 0.290822 * mu) * mu) * mu);
    let P_uniform = clamp(P_u, 0.0, 1.0);
    let P_cltc = 1.0 - P_uniform;
    var wiPdf = vec4<f32>(0.0, 0.0, 1.0, 1.0 / (2.0 * EON_PI));
    var pdf_cltc = 0.0;
    if (u1In <= P_uniform) {
        let u1 = select(0.0, u1In / max(P_uniform, 1.0e-6), P_uniform > 1.0e-6);
        let wi = eon_uniform_lobe_sample(u1, u2In);
        pdf_cltc = eon_cltc_pdf(wo_local, wi, roughness);
        wiPdf = vec4<f32>(wi, P_uniform * (1.0 / (2.0 * EON_PI)) + P_cltc * pdf_cltc);
    } else {
        let u1 = select(0.0, (u1In - P_uniform) / max(P_cltc, 1.0e-6), P_cltc > 1.0e-6);
        wiPdf = eon_cltc_sample(wo_local, roughness, u1, u2In);
        pdf_cltc = wiPdf.w;
        wiPdf.w = P_uniform * (1.0 / (2.0 * EON_PI)) + P_cltc * pdf_cltc;
    }
    return wiPdf;
}

fn eon_pdf(wo_local : vec3<f32>, wi_local : vec3<f32>, roughnessIn : f32) -> f32 {
    let roughness = clamp(roughnessIn, 0.0, 1.0);
    let mu = clamp(wo_local.z, 0.0, 1.0);
    let P_uniform = clamp(pow(max(roughness, 1.0e-6), 0.1) * (0.162925 + (-0.372058 + (0.538233 - 0.290822 * mu) * mu) * mu), 0.0, 1.0);
    let P_cltc = 1.0 - P_uniform;
    let pdf_uniform = 1.0 / (2.0 * EON_PI);
    let pdf_cltc = eon_cltc_pdf(wo_local, wi_local, roughness);
    return P_uniform * pdf_uniform + P_cltc * pdf_cltc;
}
