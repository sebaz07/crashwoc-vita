/* Dolphin MTX library subset + paired-single helpers rewritten in C. */
#include "port.h"
#include <math.h>
#include <string.h>

typedef float Mtx[3][4];
typedef float Mtx44[4][4];

void PSMTXIdentity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) { if (src != dst) memcpy(dst, src, sizeof(Mtx)); }

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx t;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
    }
    memcpy(ab, t, sizeof(Mtx));
}

void PSMTXTranspose(const Mtx src, Mtx xpose) {
    Mtx t;
    t[0][0] = src[0][0]; t[0][1] = src[1][0]; t[0][2] = src[2][0]; t[0][3] = 0.0f;
    t[1][0] = src[0][1]; t[1][1] = src[1][1]; t[1][2] = src[2][1]; t[1][3] = 0.0f;
    t[2][0] = src[0][2]; t[2][1] = src[1][2]; t[2][2] = src[2][2]; t[2][3] = 0.0f;
    memcpy(xpose, t, sizeof(Mtx));
}

unsigned int PSMTXInverse(const Mtx src, Mtx inv) {
    Mtx t;
    float det;
    det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] +
          src[0][2] * src[1][0] * src[2][1] - src[2][0] * src[1][1] * src[0][2] -
          src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    if (det == 0.0f) return 0;
    det = 1.0f / det;
    t[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * det;
    t[0][1] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * det;
    t[0][2] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * det;
    t[1][0] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * det;
    t[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * det;
    t[1][2] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * det;
    t[2][0] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * det;
    t[2][1] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * det;
    t[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * det;
    t[0][3] = -t[0][0] * src[0][3] - t[0][1] * src[1][3] - t[0][2] * src[2][3];
    t[1][3] = -t[1][0] * src[0][3] - t[1][1] * src[1][3] - t[1][2] * src[2][3];
    t[2][3] = -t[2][0] * src[0][3] - t[2][1] * src[1][3] - t[2][2] * src[2][3];
    memcpy(inv, t, sizeof(Mtx));
    return 1;
}

void PSMTXScale(Mtx m, float x, float y, float z) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = x; m[1][1] = y; m[2][2] = z;
}

/* GameCube projection: clip z ends in [-w, 0] (converted in the vertex shader). */
void C_MTXPerspective(Mtx44 m, float fovY, float aspect, float n, float f) {
    float angle = fovY * 0.5f * (float)M_PI / 180.0f;
    float cot = 1.0f / tanf(angle);
    float tmp = 1.0f / (f - n);
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = -n * tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][2] = -1.0f;
}

void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f) {
    float tmp;
    memset(m, 0, sizeof(Mtx44));
    tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][3] = -(r + l) * tmp;
    tmp = 1.0f / (t - b);
    m[1][1] = 2.0f * tmp;
    m[1][3] = -(t + b) * tmp;
    tmp = 1.0f / (f - n);
    m[2][2] = -1.0f * tmp;
    m[2][3] = -f * tmp;
    m[3][3] = 1.0f;
}

/* system/vecmatmul_asm.txt: out = M(3x4) * (v, 1) */
static void mul34(float out[3], const float *v, const float *m) {
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
    out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
    out[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
}

void VecMatMulAndWeight1(float *out, const float *v, const float *m) { mul34(out, v, m); }

/* out = w0*M1v + w1*M2v + (one - w0 - w1)*M3v */
void VecMatMulAndWeight3(float *out, const float *v, const float *m1, const float *m2, const float *m3,
                         const float *weights, const float *one) {
    float a[3], b[3], c[3];
    float w0 = weights[0], w1 = weights[1], w2 = *one - (w0 + w1);
    int i;
    mul34(a, v, m1);
    mul34(b, v, m2);
    mul34(c, v, m3);
    for (i = 0; i < 3; i++) out[i] = a[i] * w0 + b[i] * w1 + c[i] * w2;
}
